/**
 * @file dynamic_island.h
 * @brief macOS 风格灵动岛通知组件（Dear ImGui 实现）
 * @details 独立于 workx 分层之外的原型组件：接收通知 → 动画状态机
 *          （滑入/展开/停留/收缩滑出）→ DrawList 渲染圆角胶囊。
 *          宿主（app 层）负责把 EventBus 事件翻译为 Push() 调用。
 */

#pragma once

#include <cstdint>
#include <deque>
#include <string>

struct ImDrawList;

namespace di {

enum class NotifyKind {
    Info,     ///< 中性信息（蓝）
    Success,  ///< 成功（绿）
    Warning,  ///< 警告（琥珀）
    Error,    ///< 错误（红）
    Tool,     ///< 工具活动（紫）
};

struct Notification {
    uint64_t id = 0;
    NotifyKind kind = NotifyKind::Info;
    std::string title;
    std::string body;

    float enter = 0.0f;   ///< 进入动画进度 0..1（easeOutBack，轻微过冲）
    float leave = 0.0f;   ///< 离开动画进度 0..1（easeInCubic + 高度收缩）
    float alive = 0.0f;   ///< 停留计时（秒）
    float hold = 4.0f;    ///< 停留时长（点击后重置，-1 表示常驻）
    bool expanded = false; ///< 用户点击展开（显示完整正文）
    float expand = 0.0f;  ///< 展开动画进度 0..1
};

/// @brief 灵动岛容器：通知队列 + 动画推进 + 渲染
class DynamicIsland {
public:
    /// @brief 压入一条通知
    void Push(NotifyKind kind, std::string title, std::string body, float hold_sec = 4.0f);

    /// @brief 每帧推进动画，dt 秒
    void Update(float dt);

    /// @brief 绘制全部岛（x, y 为容器左上角，width 为岛宽度）
    void Draw(ImDrawList* dl, float x, float y, float width);

    /// @brief 当前全部岛的总高（含间距，动画中）
    float Height() const;

    bool Empty() const { return m_items.empty(); }
    void Clear() { m_items.clear(); }

private:
    void remove_finished();

    std::deque<Notification> m_items;
    uint64_t m_next_id = 1;
};

} // namespace di
