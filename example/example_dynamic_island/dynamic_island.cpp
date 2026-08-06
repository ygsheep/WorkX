#include "dynamic_island.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>

namespace di {

namespace {

// ---- 动画曲线 ----

inline float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

/// easeOutCubic：进入主体
inline float ease_out_cubic(float t) {
    t = 1.0f - t;
    return 1.0f - t * t * t;
}

/// easeOutBack：进入末端轻微过冲（macOS 弹性感）
inline float ease_out_back(float t, float overshoot = 0.35f) {
    const float c1 = 1.70158f + overshoot;
    const float c3 = c1 + 1.0f;
    t -= 1.0f;
    return 1.0f + c3 * t * t * t + c1 * t * t;
}

/// easeInCubic：离开加速
inline float ease_in_cubic(float t) { return t * t * t; }

/// 类型 → 主题色
ImVec4 kind_color(NotifyKind kind) {
    switch (kind) {
    case NotifyKind::Success: return ImVec4(0.31f, 0.78f, 0.47f, 1.0f);
    case NotifyKind::Warning: return ImVec4(0.95f, 0.72f, 0.26f, 1.0f);
    case NotifyKind::Error:   return ImVec4(0.91f, 0.36f, 0.31f, 1.0f);
    case NotifyKind::Tool:    return ImVec4(0.66f, 0.53f, 0.96f, 1.0f);
    case NotifyKind::Info:
    default:                  return ImVec4(0.36f, 0.65f, 0.95f, 1.0f);
    }
}

/// 类型 → 图标字符（黑体覆盖常用符号）
const char* kind_icon(NotifyKind kind) {
    switch (kind) {
    case NotifyKind::Success: return "\u2713"; // ✓
    case NotifyKind::Warning: return "\u26A0"; // ⚠
    case NotifyKind::Error:   return "\u2715"; // ✕
    case NotifyKind::Tool:    return "\u21BB"; // ↻
    case NotifyKind::Info:
    default:                  return "\u2139"; // ℹ
    }
}

constexpr float kIslandCorner = 22.0f; ///< 胶囊圆角
constexpr float kCollapsedH = 46.0f;   ///< 收起高度（标题行）
constexpr float kExpandedH = 84.0f;    ///< 展开高度（标题 + 两行正文）
constexpr float kSpacing = 10.0f;      ///< 岛间距

} // namespace

void DynamicIsland::Push(NotifyKind kind, std::string title, std::string body, float hold_sec) {
    Notification n;
    n.id = m_next_id++;
    n.kind = kind;
    n.title = std::move(title);
    n.body = std::move(body);
    n.hold = hold_sec;
    m_items.push_front(std::move(n)); // 新通知置顶
    if (m_items.size() > 5) m_items.pop_back(); // 上限 5 条
}

void DynamicIsland::Update(float dt) {
    for (auto& n : m_items) {
        if (n.leave <= 0.0f) {
            // 进入阶段
            if (n.enter < 1.0f) {
                n.enter = std::min(1.0f, n.enter + dt / 0.35f);
            } else if (n.hold >= 0.0f) {
                // 停留
                n.alive += dt;
                if (n.alive >= n.hold) n.leave = 0.001f; // 触发离开
            }
        } else {
            // 离开阶段（easeInCubic 0.3s）
            n.leave = std::min(1.0f, n.leave + dt / 0.30f);
        }
        // 展开动画
        const float target_expand = n.expanded ? 1.0f : 0.0f;
        n.expand += (target_expand - n.expand) * std::min(1.0f, dt * 12.0f);
        if (std::abs(target_expand - n.expand) < 0.001f) n.expand = target_expand;
    }
    remove_finished();
}

void DynamicIsland::remove_finished() {
    std::erase_if(m_items, [](const Notification& n) { return n.leave >= 1.0f; });
}

float DynamicIsland::Height() const {
    float h = 0.0f;
    for (const auto& n : m_items) {
        if (n.leave > 0.0f) h += kCollapsedH * (1.0f - ease_in_cubic(n.leave));
        else h += n.enter * kCollapsedH;
        h += kSpacing;
    }
    return h;
}

void DynamicIsland::Draw(ImDrawList* dl, float x, float y, float width) {
    for (auto& n : m_items) {
        const float leave_t = ease_in_cubic(n.leave);
        const float alpha = (1.0f - leave_t) * ease_out_cubic(n.enter);

        if (alpha <= 0.0f) continue;

        const float island_h =
            kCollapsedH + (kExpandedH - kCollapsedH) * n.expand;
        const float shown_h = n.leave > 0.0f
            ? island_h * (1.0f - leave_t)
            : island_h * ease_out_back(n.enter);
        if (shown_h <= 1.0f) continue;

        // 滑入偏移：进入时从上方 24px 滑落，离开时上滑 16px
        const float slide = n.leave > 0.0f
            ? -16.0f * leave_t
            : 24.0f * (1.0f - ease_out_cubic(n.enter));

        const ImVec2 pmin(x, y + slide);
        const ImVec2 pmax(x + width, y + slide + shown_h);

        ImGui::PushClipRect(pmin, pmax, true);

        const ImU32 bg = IM_COL32(22, 22, 28, (int)(235 * alpha));
        const ImU32 bg_hover = IM_COL32(34, 34, 44, (int)(235 * alpha));

        const bool hovered = ImGui::IsMouseHoveringRect(pmin, pmax);
        const bool clicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        if (clicked) {
            n.expanded = !n.expanded;
            n.alive = 0.0f; // 重置停留计时
        }

        // 阴影：多层半透明圆角叠加模拟（1.91.9 无 AddShadowRect）
        if (hovered) {
            const ImU32 shadow = IM_COL32(0, 0, 0, (int)(90 * alpha));
            dl->AddRectFilled(ImVec2(pmin.x - 4, pmin.y - 4),
                              ImVec2(pmax.x + 4, pmax.y + 4), shadow, kIslandCorner + 4);
            dl->AddRectFilled(ImVec2(pmin.x - 1, pmin.y - 1),
                              ImVec2(pmax.x + 1, pmax.y + 1), shadow, kIslandCorner + 1);
        }
        dl->AddRectFilled(pmin, pmax, hovered ? bg_hover : bg, kIslandCorner);

        // 左侧类型色条（小圆角胶囊）
        const ImVec4 kc = kind_color(n.kind);
        const ImU32 kcu = ImGui::ColorConvertFloat4ToU32(
            ImVec4(kc.x, kc.y, kc.z, kc.w * alpha));
        dl->AddRectFilled(ImVec2(pmin.x + 8, pmin.y + 12),
                          ImVec2(pmin.x + 5, pmax.y - 12), kcu, 2.5f);

        ImGui::SetCursorScreenPos(ImVec2(pmin.x + 18, pmin.y + 4));

        // 标题行：图标 + 标题（粗体）
        ImGui::PushFont(ImGui::GetIO().Fonts->Fonts.Size > 2
                            ? ImGui::GetIO().Fonts->Fonts[2]
                            : ImGui::GetFont());
        ImGui::TextColored(ImVec4(1, 1, 1, alpha), "%s  %s",
                           kind_icon(n.kind), n.title.c_str());
        ImGui::PopFont();

        // 正文（展开时最多两行）
        if (n.expand > 0.05f && !n.body.empty()) {
            ImGui::PushFont(ImGui::GetIO().Fonts->Fonts.Size > 1
                                ? ImGui::GetIO().Fonts->Fonts[1]
                                : ImGui::GetFont());
            ImGui::SetCursorScreenPos(ImVec2(pmin.x + 18, pmin.y + 30));
            ImGui::PushTextWrapPos(pmax.x - 14);
            ImGui::TextColored(ImVec4(0.78f, 0.78f, 0.85f, alpha), "%s", n.body.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopFont();
        }

        // 右上角：停留倒计时（细条）
        if (n.hold > 0.0f && n.leave <= 0.0f) {
            const float remain = 1.0f - clamp01(n.alive / n.hold);
            dl->AddRectFilled(ImVec2(pmin.x + 18, pmax.y - 6),
                              ImVec2(pmin.x + 18 + (width - 36) * remain,
                                     pmax.y - 4.5f),
                              IM_COL32(255, 255, 255, (int)(45 * alpha)), 1.0f);
        }

        ImGui::PopClipRect();

        y += shown_h + kSpacing;
    }
}

} // namespace di
