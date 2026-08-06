/**
 * @file event_bridge.h
 * @brief EventBus → 灵动岛通知桥（app 层宿主组件）
 * @details 订阅 agent 宿主无关事件（工具调用/结果、任务完成、AskUser 请求、
 *          后端状态、压缩暂停），翻译为通知入队；发布线程回调仅加锁入队，
 *          主循环 Drain() 取出。不引入 core/agent 到 UI 的依赖。
 */

#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "core/events/event_token.h"
#include "core/events/i_event_bus.h"
#include "dynamic_island.h"  // NotifyKind

namespace di {

struct IslandMessage {
    NotifyKind kind = NotifyKind::Info;
    std::string title;
    std::string body;
};

class EventBridge {
public:
    explicit EventBridge(agent::IEventBus& bus);
    ~EventBridge() = default;

    EventBridge(const EventBridge&) = delete;
    EventBridge& operator=(const EventBridge&) = delete;

    /// @brief 取出所有待显示通知（主循环每帧调用）
    void Drain(std::vector<IslandMessage>& out);

private:
    /// @brief 订阅（原型：不保存释放器。EventBus 为进程级单例，
    ///        生命周期长于本桥，进程退出即清理，无需显式退订）
    template<typename T>
    void subscribe(std::function<void(const T&)> cb) {
        m_bus.template subscribe<T>(std::move(cb));
    }

    void enqueue(NotifyKind kind, std::string title, std::string body);

    agent::IEventBus& m_bus;
    std::mutex m_mutex;
    std::vector<IslandMessage> m_queue;
};

} // namespace di
