/**
 * @file icons.h
 * @brief 图标层：Nerd Font 私有区字符 ↔ ASCII 降级（A5 可访问性）
 * @details Nerd Font 图标（U+F000 私有区）在无对应字体的终端显示为豆腐块。
 *          提供全局开关（默认开启），关闭后所有私有区字符降级为 ASCII/空。
 *          Braille 旋转符、✓/✖ 等通用 Unicode 不在此列（多数终端支持）。
 */

#pragma once

#include <string_view>

namespace ftxtui::theme {

/// @brief 内部标志（static 局部，避免头文件 ODR 问题）
inline bool& nerd_font_flag() {
    static bool flag = true;
    return flag;
}

/// @brief 设置 Nerd Font 图标开关（启动时由 main 解析 --ascii 设置）
inline void set_nerd_font(bool enabled) { nerd_font_flag() = enabled; }

/// @brief Nerd Font 图标是否开启
inline bool nerd_font() { return nerd_font_flag(); }

/// @brief 取图标：开启时用 Nerd Font 字形，否则用 ASCII 降级
inline std::string_view icon(std::string_view nf, std::string_view ascii) {
    return nerd_font() ? nf : ascii;
}

// ----------------------------------------------------------------------------
// 常用图标（Nerd Font 私有区 → ASCII 降级）
// ----------------------------------------------------------------------------

/// @brief 思考（灯泡 nf-fa-lightbulb）→ 无图标
inline std::string_view icon_think() { return icon("\uF0EB", ""); }
/// @brief 展开指示（chevron-down）→ "v"
inline std::string_view icon_chevron_down() { return icon("\uF078", "v"); }
/// @brief 收起指示（chevron-right）→ ">"
inline std::string_view icon_chevron_right() { return icon("\uF054", ">"); }
/// @brief 工具（扳手 nf-fa-wrench）→ 无图标
inline std::string_view icon_tool() { return icon("\uF0AD", ""); }
/// @brief 计划模式（剪贴板 nf-fa-clipboard）→ "P"
inline std::string_view icon_plan() { return icon("\uF044", "P"); }
/// @brief 完全访问（key nf-cod-key）→ "A"
inline std::string_view icon_full_access() { return icon("\uEB53", "A"); }
/// @brief 手动审批（hand nf-fa-hand-paper）→ "?"
inline std::string_view icon_manual() { return icon("\uF256", "?"); }
/// @brief 复制（nf-fa-copy）→ "C"
inline std::string_view icon_copy() { return icon("\uF0C5", "C"); }
/// @brief 重试（nf-fa-rotate-right）→ "R"
inline std::string_view icon_retry() { return icon("\uF2F9", "R"); }
/// @brief 状态点（nf-cod-circle_small_filled，小实心点）→ "·"（MCP 连接状态：绿/红/灰）
inline std::string_view icon_dot() { return icon("\uEB8A", "·"); }

}  // namespace ftxtui::theme