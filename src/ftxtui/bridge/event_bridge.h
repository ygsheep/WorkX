/**
 * @file event_bridge.h
 * @brief EventBridge — EventBus 订阅 → ActionQueue 映射
 * @details 事件回调（可能位于后台/事件泵线程）只入队 action；UI 线程每帧 drain。
 *          不持锁碰 UI 状态。见设计文档 §3。
 */

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/events/i_event_bus.h"
#include "core/events/event_token.h"

#include "bridge/action_queue.h"

namespace ftxtui {

/// @brief 事件桥
class EventBridge {
public:
    /// @param bus 事件总线（非拥有）
    /// @param queue 动作队列（非拥有；事件回调只向它入队）
    EventBridge(agent::IEventBus& bus, ActionQueue& queue)
        : m_bus(bus), m_queue(queue) {}

    /// @brief 订阅全部 UI 相关事件
    void start();

    /// @brief 退订并在成功后清空桥内引用
    void stop();

    /// @brief 设置通知回调：入队后调用，用于唤醒 UI 线程重绘（screen.PostEvent）
    void set_wake_callback(std::function<void()> cb) { m_wake = std::move(cb); }

    /// @brief 入队一个动作（线程安全）
    void push(Action action);

private:
    agent::IEventBus& m_bus;
    ActionQueue& m_queue;
    std::function<void()> m_wake;

    std::vector<agent::EventToken> m_tokens;
};

}  // namespace ftxtui