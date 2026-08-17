/**
 * @file command_palette.h
 * @brief 命令面板：搜索 + 列表选择（类似 opencode 效果）
 * @details 悬浮居中窗口，顶部搜索框实时过滤命令，下方列表选择。
 *          搜索命中「标题 + 命令 + 关键词」（中英文均可，如"切换模型"/"switch model"）。
 *          Esc 先清空搜索、再关闭；Up/Down / Ctrl+N / Ctrl+P 移动选择；
 *          Enter 运行选中项（回调为 commands 的原始下标）。
 */

#pragma once

#include <functional>
#include <string>
#include <vector>

#include <ftxui/component/component.hpp>

namespace ftxtui {

/// @brief 命令面板条目（从 agent 命令注册表派生；保持 UI 侧数据模型独立）
struct PaletteCommand {
    std::string command;   ///< 实际执行命令，如 "/model"
    std::string title;     ///< 显示标题，如 "切换模型"
    std::string keywords;  ///< 额外搜索关键词（中英文/别名），可空
};

/// @brief 构建命令面板组件（内部自带搜索 + 过滤，打开后自动聚焦）
/// @param commands 全部命令条目（调用方按原始顺序；面板内部做子串搜索过滤）
/// @param on_select 选中回调（Enter 运行；参数为 commands 的原始下标）
/// @param open 开关引用（Esc / Enter 关闭时置 false）
/// @param on_close 关闭回调（用于恢复焦点等；可为空）
ftxui::Component make_command_palette(
    std::vector<PaletteCommand> commands,
    std::function<void(int)> on_select,
    bool& open,
    std::function<void()> on_close = nullptr);

}  // namespace ftxtui