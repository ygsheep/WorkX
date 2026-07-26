/**
 * @file event_bus.h
 * @brief 类型安全的事件总线 + RAII 事件守卫
 * @details Header-only，无外部依赖
 * @version 1.0.0
 */

#pragma once

#include <functional>
#include <vector>
#include <mutex>
#include <unordered_map>
#include <typeindex>
#include <cstdint>

#include "liblogger/logger.h"

namespace agent {

class EventToken {
public:
    using ID = uint64_t;

    EventToken() : m_id(0), m_is_valid(false) {}
    explicit EventToken(ID id) : m_id(id), m_is_valid(true) {}

    EventToken(const EventToken&) = default;
    EventToken& operator=(const EventToken&) = default;

    EventToken(EventToken&& other) noexcept
        : m_id(other.m_id), m_is_valid(other.m_is_valid) {
        other.m_is_valid = false;
        other.m_id = 0;
    }

    EventToken& operator=(EventToken&& other) noexcept {
        if (this != &other) {
            m_id = other.m_id;
            m_is_valid = other.m_is_valid;
            other.m_is_valid = false;
            other.m_id = 0;
        }
        return *this;
    }

    [[nodiscard]] bool is_valid() const { return m_is_valid; }
    [[nodiscard]] ID get_id() const { return m_id; }

    void invalidate() { m_is_valid = false; }

private:
    ID m_id;
    bool m_is_valid;
};

class EventBus final {
public:
    static EventBus& instance() noexcept {
        static EventBus inst;
        return inst;
    }

    EventBus(const EventBus&) = delete;
    EventBus& operator=(const EventBus&) = delete;
    EventBus(EventBus&&) = delete;
    EventBus& operator=(EventBus&&) = delete;

    template<typename T>
    [[nodiscard]] EventToken subscribe(std::function<void(const T&)> callback) {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto token = EventToken(m_next_token_id++);

        auto& callbacks = m_callbacks[typeid(T)];
        callbacks.push_back(CallbackWrapper{
            .callback = [callback](const void* event) {
                callback(*static_cast<const T*>(event));
            },
            .token_id = token.get_id()
        });

        return token;
    }

    template<typename T>
    void unsubscribe(const EventToken& token) {
        if (!token.is_valid()) return;

        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_callbacks.find(typeid(T));
        if (it != m_callbacks.end()) {
            auto& callbacks = it->second;
            callbacks.erase(
                std::remove_if(callbacks.begin(), callbacks.end(),
                    [&token](const CallbackWrapper& wrapper) {
                        return wrapper.token_id == token.get_id();
                    }),
                callbacks.end()
            );
        }
    }

    template<typename T>
    void publish(const T& event) {
        // 拷贝回调列表，避免持锁调用用户代码（防止死锁与重入问题）
        std::vector<CallbackWrapper> callbacks_copy;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto it = m_callbacks.find(typeid(T));
            if (it == m_callbacks.end()) return;
            callbacks_copy = it->second;
        }

        for (const auto& wrapper : callbacks_copy) {
            try {
                wrapper.callback(&event);
            } catch (const std::exception&) {
                // 吞掉标准异常，不中断其他回调
            } catch (...) {
                // 兜底：吞掉所有异常，防止 terminate
            }
        }
    }

    template<typename T>
    void publish_async(const T& event) {
        std::lock_guard<std::mutex> lock(m_async_mutex);
        // 不捕获 this，通过 instance() 访问单例
        m_async_queue.push_back([event]() {
            EventBus::instance().publish(event);
        });
        // G-1 日志：记录事件入队与队列积压
        const size_t queue_size = m_async_queue.size();
        if (queue_size > 100) {
            LOG_WARN("[event={}] publish_async backlog: {} events",
                     typeid(T).name(), queue_size);
        } else {
            LOG_DEBUG("[event={}] publish_async enqueue, queue_size={}",
                      typeid(T).name(), queue_size);
        }
    }

    void process_async_events() {
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

    void clear() {
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

    // === 调试 / 诊断接口（G-1）===
    // 用于测试失败时输出 EventBus 内部状态，便于定位问题

    /// @brief 异步事件队列当前积压数量
    [[nodiscard]] size_t async_queue_size() const {
        std::lock_guard<std::mutex> lock(m_async_mutex);
        return m_async_queue.size();
    }

    /// @brief 指定事件类型的订阅者数量
    template<typename T>
    [[nodiscard]] size_t subscriber_count() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_callbacks.find(typeid(T));
        return it != m_callbacks.end() ? it->second.size() : 0;
    }

    /// @brief 全部事件类型的订阅者总数
    [[nodiscard]] size_t total_subscriber_count() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        size_t total = 0;
        for (const auto& [_, callbacks] : m_callbacks) {
            total += callbacks.size();
        }
        return total;
    }

private:
    EventBus() = default;
    ~EventBus() = default;

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
