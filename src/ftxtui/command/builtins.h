/**
 * @file builtins.h
 * @brief FTXUI TUI 内置命令注册（B2 统一命令）
 * @details 把内置命令（help/exit/clear/model/provider/resume/rename）注册为
 *          agent::command::LocalCommand 到 agent 侧 CommandRegistry，
 *          使 App 成为唯一执行入口、命令定义单一（不再有第二套注册表）。
 *          命令的 UI 副作用（开模型面板/退出/清空/恢复/重命名）经回调触发。
 */

#pragma once

#include <functional>
#include <memory>

#include "agent/command/inclaude/registry.h"

namespace ftxtui {

/// @brief 内置命令所需的 UI 副作用回调
/// @details App 注入（捕获 ViewModel / m_screen 等）；注册表侧只持有这些回调。
struct FtuiCommandCallbacks {
    std::function<void()> on_exit;          ///< /exit：请求退出 UI
    std::function<void()> on_model_select;  ///< /model：打开模型选择面板
    std::function<void()> on_provider_select; ///< /provider：打开供应商切换面板
    /// @brief /resume：恢复历史会话（args 为空打开会话面板、非空切换）
    std::function<void(const std::string&)> on_resume;
    /// @brief /rename：重命名会话（args 为标题）
    std::function<void(const std::string&)> on_rename;
    /// @brief /clear：清空会话
    std::function<void()> on_clear;
};

/// @brief 把内置命令注册进 agent 命令注册表
/// @param registry 目标注册表（非空）
/// @param cb 命令副作用回调
void register_ftx_builtins(agent::command::CommandRegistry& registry,
                           const FtuiCommandCallbacks& cb);

}  // namespace ftxtui
