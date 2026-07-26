/**
 * @file i_event_bus.h
 * @brief 事件总线抽象接口（D-1 DI 化）
 * @details 类型擦除的虚函数接口 + 模板包装，允许测试注入 MockEventBus，
 *          解除对 EventBus 单例的硬依赖。
 *          类型擦除虚函数使用 _raw 后缀，避免与模板包装同名导致 MSVC 解析歧义。
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <functional>
#include <typeindex>
#include <cstddef>

#include "core/events/event_token.h"

namespace agent {

/// @brief 事件总线抽象接口
/// @details 提供类型擦除的虚函数接口供 DI 注入；模板包装方法委托虚函数，
///          调用方可像使用 EventBus 一样使用 IEventBus&。
class IEventBus {
public:
    virtual ~IEventBus() = default;

    // === 类型擦除的虚函数（由实现类提供，使用 _raw 后缀避免与模板包装冲突）===

    /// @brief 订阅事件（类型擦除）
    /// @param type 事件类型（typeid(T)）
    /// @param callback 回调函数，接收 const void* 指向事件对象
    /// @return 订阅令牌
    virtual EventToken subscribe_raw(std::type_index type,
                                     std::function<void(const void*)> callback) = 0;

    /// @brief 取消订阅
    virtual void unsubscribe_raw(std::type_index type, const EventToken& token) = 0;

    /// @brief 同步发布事件
    virtual void publish_raw(std::type_index type, const void* event) = 0;

    /// @brief 异步发布事件（入队，等 process_async_events 处理）
    /// @param emitter 实际发布事件的函数对象（捕获事件拷贝并调用 publish_raw）
    virtual void publish_async_raw(std::type_index type,
                                   std::function<void()> emitter) = 0;

    /// @brief 处理所有异步事件
    virtual void process_async_events() = 0;

    /// @brief 清空所有订阅与异步队列
    virtual void clear() = 0;

    // === 诊断接口 ===

    [[nodiscard]] virtual size_t async_queue_size() const = 0;
    [[nodiscard]] virtual size_t total_subscriber_count() const = 0;

    // === 模板包装（非虚，委托 _raw 虚函数）===

    /// @brief 订阅事件（类型安全）
    template<typename T>
    EventToken subscribe(std::function<void(const T&)> callback) {
        return subscribe_raw(std::type_index(typeid(T)),
            [cb = std::move(callback)](const void* p) {
                cb(*static_cast<const T*>(p));
            });
    }

    /// @brief 取消订阅（类型安全）
    template<typename T>
    void unsubscribe(const EventToken& token) {
        unsubscribe_raw(std::type_index(typeid(T)), token);
    }

    /// @brief 同步发布事件（类型安全）
    template<typename T>
    void publish(const T& event) {
        publish_raw(std::type_index(typeid(T)), &event);
    }

    /// @brief 异步发布事件（类型安全）
    /// @details 捕获事件拷贝到 emitter，等 process_async_events 时再同步发布
    template<typename T>
    void publish_async(const T& event) {
        publish_async_raw(std::type_index(typeid(T)),
            [this, event]() { this->publish_raw(std::type_index(typeid(T)), &event); });
    }

    /// @brief 指定事件类型的订阅者数量
    template<typename T>
    [[nodiscard]] size_t subscriber_count() const {
        return subscriber_count_typed(std::type_index(typeid(T)));
    }

protected:
    /// @brief 类型擦除的 subscriber_count（供模板包装调用）
    [[nodiscard]] virtual size_t subscriber_count_typed(std::type_index type) const = 0;
};

} // namespace agent
