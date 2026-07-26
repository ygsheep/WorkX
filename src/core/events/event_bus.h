/**
 * @file event_bus.h
 * @brief 类型安全的事件总线 + RAII 事件守卫
 * @details Header-only，无外部依赖。继承 IEventBus 支持 DI 注入。
 * @version 2.0.0
 */

#pragma once

#include <functional>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <typeindex>
#include <cstdint>

#include "liblogger/logger.h"
#include "core/events/event_token.h"
#include "core/events/i_event_bus.h"

namespace agent {

class EventBus final : public IEventBus {
public:
    static EventBus& instance() noexcept {
        static EventBus inst;
        return inst;
    }

    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;
    EventBus(EventBus&&) = delete;
    EventBus& operator=(EventBus&&) = delete;

    // === IEventBus 类型擦除接口实现 ===

    EventToken subscribe_raw(std::type_index type,
                             std::function<void(const void*)> callback) override {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto token = EventToken(m_next_token_id++);

        auto& callbacks = m_callbacks[type];
        callbacks.push_back(CallbackWrapper{
            .callback = std::move(callback),
            .token_id = token.get_id()
        });

        LOG_DEBUG("[event={}] subscribe, token={}, total_subscribers={}",
                  type.name(), token.get_id(), callbacks.size());
        return token;
    }

    void unsubscribe_raw(std::type_index type, const EventToken& token) override {
        if (!token.is_valid()) return;

        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_callbacks.find(type);
        if (it != m_callbacks.end()) {
            auto& callbacks = it->second;
            callbacks.erase(
                std::remove_if(callbacks.begin(), callbacks.end(),
                    [&token](const CallbackWrapper& wrapper) {
                        return wrapper.token_id == token.get_id();
                    }),
                callbacks.end()
            );
            LOG_DEBUG("[event={}] unsubscribe, token={}, remaining={}",
                      type.name(), token.get_id(), callbacks.size());
        }
    }

    void publish_raw(std::type_index type, const void* event) override {
        // T-1 修复：拷贝回调列表，避免持锁调用用户代码（防止死锁与重入问题）
        std::vector<CallbackWrapper> callbacks_copy;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_callbacks.find(type);
            if (it == m_callbacks.end()) return;
            callbacks_copy = it->second;
        }

        for (const auto& wrapper : callbacks_copy) {
            try {
                wrapper.callback(event);
            } catch (const std::exception&) {
                // 吞掉标准异常，不中断其他回调
            } catch (...) {
                // 兜底：吞掉所有异常，防止 terminate
            }
        }
    }

    void publish_async_raw(std::type_index type,
                           std::function<void()> emitter) override {
        std::lock_guard<std::mutex> lock(m_async_mutex);
        m_async_queue.push_back(std::move(emitter));
        // G-1 日志：记录事件入队与队列积压
        const size_t queue_size = m_async_queue.size();
        if (queue_size > 100) {
            LOG_WARN("[event={}] publish_async backlog: {} events",
                     type.name(), queue_size);
        } else {
            LOG_DEBUG("[event={}] publish_async enqueue, queue_size={}",
                      type.name(), queue_size);
        }
    }

    void process_async_events() override {
        std::vector<std::function<void()>> queue_copy;
        {
            std::lock_guard<std::mutex> lock(m_async_mutex);
            queue_copy = std::move(m_async_queue);
            m_async_queue.clear();
        }
        if (!queue_copy.empty()) {
            LOG_DEBUG("process {} async events, remaining={}",
                      queue_copy.size(), m_async_queue.size());
        }
        for (auto& callback : queue_copy) {
            callback();
        }
    }

    void clear() override {
        size_t subscriber_count = 0;
        size_t queue_size = 0;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (const auto& [_, callbacks] : m_callbacks) {
                subscriber_count += callbacks.size();
            }
            m_callbacks.clear();
        }
        {
            std::lock_guard<std::mutex> lock(m_async_mutex);
            queue_size = m_async_queue.size();
            m_async_queue.clear();
        }
        LOG_INFO("EventBus cleared: {} subscribers, {} async events dropped",
                 subscriber_count, queue_size);
    }

    // === 诊断接口 ===

    [[nodiscard]] size_t async_queue_size() const override {
        std::lock_guard<std::mutex> lock(m_async_mutex);
        return m_async_queue.size();
    }

    [[nodiscard]] size_t total_subscriber_count() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        size_t total = 0;
        for (const auto& [_, callbacks] : m_callbacks) {
            total += callbacks.size();
        }
        return total;
    }

protected:
    [[nodiscard]] size_t subscriber_count_typed(std::type_index type) const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_callbacks.find(type);
        return it != m_callbacks.end() ? it->second.size() : 0;
    }

private:
    EventBus() = default;
    ~EventBus() override = default;

    struct CallbackWrapper {
        std::function<void(const void*)> callback;
        EventToken::ID token_id;
    };

    mutable std::mutex m_mutex;
    EventToken::ID m_next_token_id = 1;
    std::unordered_map<std::type_index, std::vector<CallbackWrapper>> m_callbacks;

    mutable std::mutex m_async_mutex;
    std::vector<std::function<void()>> m_async_queue;
};

template<typename T>
class EventGuard {
public:
    using CallbackType = std::function<void(const T&)>;

    explicit EventGuard(CallbackType callback)
        : m_token(EventBus::instance().subscribe<T>(std::move(callback)))
    {}

    ~EventGuard() {
        EventBus::instance().unsubscribe<T>(m_token);
    }

    EventGuard(const EventGuard&) = delete;
    EventGuard& operator=(const EventGuard&) = delete;

    EventGuard(EventGuard&& other) noexcept
        : m_token(std::move(other.m_token))
    {
        other.m_token.invalidate();
    }

    EventGuard& operator=(EventGuard&& other) noexcept {
        if (this != &other) {
            EventBus::instance().unsubscribe<T>(m_token);
            m_token = std::move(other.m_token);
            other.m_token.invalidate();
        }
        return *this;
    }

private:
    EventToken m_token;
};

template<typename T>
[[nodiscard]] EventGuard<T> make_event_guard(std::function<void(const T&)> callback) {
    return EventGuard<T>(std::move(callback));
}

} // namespace agent
