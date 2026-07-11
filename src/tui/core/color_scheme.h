/**
 * @file color_scheme.h
 * @brief 颜色方案
 * @details ColorRole 枚举和 ANSI 颜色映射
 * @version 2.0.0
 */

#pragma once

#include <string_view>

namespace workx {

/**
 * @brief 颜色角色
 */
enum class ColorRole {
    // 基础角色
    Default,
    Prompt,
    UserInput,
    Assistant,
    Reasoning,
    System,
    Error,
    Command,
    ToolName,
    ToolOutput,
    Progress,

    // TUI 2.0 新增
    TextColor,
    StatusBar,           ///< 状态栏文字（灰白）
    StatusBarBg,         ///< 状态栏背景（深灰 237）
    ThinkingIndicator,   ///< 思考动画字符（青色）
    ThinkingBlock,       ///< 思考块边框（灰色）
    Bullet,              ///< 结构化输出圆点（青色）
    Dim,                 ///< 暗淡文字（次要信息）
    CodeBlock,           ///< 代码块边框（灰色）
    CodeBlockBg,         ///< 代码块背景（更深灰 236）
    Success,             ///< 成功标记 ✓（绿色）
    Failure,             ///< 失败标记 ✗（红色）
    TokenStats,          ///< Token 统计信息（紫色）
    ContextWarning,      ///< 上下文使用率警告（红色）
    SplashLogo,          ///< Splash logo（青色）
    SplashInfo,          ///< Splash 信息（紫色）

    // CommandPanel
    CommandPanelDesc,    ///< 命令面板描述文字（灰色）
    CommandPanelHighlight, ///< 命令面板选中行高亮（黄色）

    // SelectPanel
    SelectTab,           ///< 选择面板 Tab 标签（灰色）
    SelectTabActive,     ///< 选择面板活跃 Tab（黄色高亮）
    SelectCursor,        ///< 选择面板光标 ●（青色）
    SelectChecked,       ///< 选择面板已选中 ◉（绿色）
    SelectUnchecked,     ///< 选择面板未选中 ○（灰色）

};

/**
 * @brief 获取颜色角色对应的 ANSI 转义序列
 */
constexpr std::string_view get_color_ansi(ColorRole role) {
    switch (role) {
        case ColorRole::Default:           return "\x1b[0m";
        case ColorRole::Prompt:            return "\x1b[33m";           // 黄色
        case ColorRole::UserInput:         return "\x1b[48;5;236m";   // 深灰背景
        case ColorRole::Assistant:         return "\x1b[36m";           // 青色
        case ColorRole::Reasoning:         return "\x1b[90m";           // 灰色
        case ColorRole::System:            return "\x1b[35m";           // 紫色
        case ColorRole::Error:             return "\x1b[1m\x1b[31m";   // 粗体红色
        case ColorRole::Command:           return "\x1b[33m";           // 黄色
        case ColorRole::ToolName:          return "\x1b[34m";           // 蓝色
        case ColorRole::ToolOutput:        return "\x1b[90m";           // 灰色
        case ColorRole::Progress:          return "\x1b[35m";           // 紫色
        case ColorRole::StatusBar:         return "\x1b[37m";           // 白灰
        case ColorRole::StatusBarBg:       return "\x1b[48;5;237m";    // 深灰背景
        case ColorRole::ThinkingIndicator: return "\x1b[36m";           // 青色
        case ColorRole::ThinkingBlock:     return "\x1b[90m";           // 亮灰
        case ColorRole::Bullet:            return "\x1b[36m";           // 青色
        case ColorRole::Dim:               return "\x1b[2m";            // 暗淡
        case ColorRole::CodeBlock:         return "\x1b[90m";           // 亮灰
        case ColorRole::CodeBlockBg:       return "\x1b[48;5;236m";    // 更深灰背景
        case ColorRole::Success:           return "\x1b[32m";           // 绿色
        case ColorRole::Failure:           return "\x1b[31m";           // 红色
        case ColorRole::TokenStats:        return "\x1b[35m";           // 紫色
        case ColorRole::ContextWarning:    return "\x1b[1m\x1b[31m";   // 粗体红色
        case ColorRole::SplashLogo:        return "\x1b[36m";           // 青色
        case ColorRole::SplashInfo:        return "\x1b[35m";           // 紫色
        case ColorRole::CommandPanelDesc:     return "\x1b[90m";       // 灰色
        case ColorRole::CommandPanelHighlight:return "\x1b[33m";       // 黄色
        case ColorRole::SelectTab:            return "\x1b[90m";       // 灰色
        case ColorRole::SelectTabActive:      return "\x1b[33m";       // 黄色
        case ColorRole::SelectCursor:         return "\x1b[36m";       // 青色
        case ColorRole::SelectChecked:        return "\x1b[32m";       // 绿色
        case ColorRole::SelectUnchecked:      return "\x1b[90m";       // 灰色
        case ColorRole::TextColor:            return "\x1b[97m";   // 亮白色
        default:                           return "\x1b[0m";
    }
}

} // namespace workx
