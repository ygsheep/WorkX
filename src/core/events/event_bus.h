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
        std::lock_guard<std::recursive_mutex> lock(m_mutex);

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

        std::lock_guard<std::recursive_mutex> lock(m_mutex);

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
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        auto it = m_callbacks.find(typeid(T));
        if (it == m_callbacks.end()) return;

        for (const auto& wrapper : it->second) {
            try {
                wrapper.callback(&event);
            } catch (const std::exception&) {
                // 吞掉异常，不中断其他回调
            }
        }
    }

    template<typename T>
    void publish_async(const T& event) {
        std::lock_guard<std::mutex> lock(m_async_mutex);
        m_async_queue.push_back([this, event]() {
            this->publish(event);
        });
    }

    void process_async_events() {
        std::vector<std::function<void()>> queue_copy;
        {
            std::lock_guard<std::mutex> lock(m_async_mutex);
            queue_copy = std::move(m_async_queue);
            m_async_queue.clear();
        }
        for (auto& callback : queue_copy) {
            callback();
        }
    }

    void clear() {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_callbacks.clear();
    }

private:
    EventBus() = default;
    ~EventBus() = default;

    struct CallbackWrapper {
        std::function<void(const void*)> callback;
        EventToken::ID token_id;
    };

    std::recursive_mutex m_mutex;
    EventToken::ID m_next_token_id = 1;
    std::unordered_map<std::type_index, std::vector<CallbackWrapper>> m_callbacks;

    std::mutex m_async_mutex;
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

} // namespace workx
