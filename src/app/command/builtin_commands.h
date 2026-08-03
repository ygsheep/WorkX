/**
 * @file builtin_commands.h
 * @brief 内置系统命令注册
 * @details 注册 help/exit/quit/clear/regen/model 等系统命令到 CommandRegistry
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <memory>
#include <functional>
#include <string>

#include "agent/command/inclaude/registry.h"

namespace agent {

class ChatSession;

namespace command {

/// 系统命令注册所需的上下文
/// 各命令通过 lambda 捕获所需的依赖
struct SystemCommandContext {
    /// 会话引用（clear/regen/rename 需要）。
    /// 指向装配层持有的 unique_ptr<ChatSession> 变量本身，而非拷贝裸指针：
    /// /provider 热切换会整体替换 session（旧对象析构），拷贝的裸指针会悬垂，
    /// 间接引用在替换后自动跟随新 session。
    std::unique_ptr<ChatSession>* session = nullptr;
    std::function<void()> on_exit;        ///< 退出回调（exit/quit 触发）
    std::function<void()> on_model_select; ///< 模型选择回调（model 触发）
    std::function<void()> on_provider_select; ///< 供应商选择回调（provider 触发）
    std::function<void()> on_resume;      ///< 会话恢复回调（resume 触发，打开 TUI 选择面板）
};

/// 注册内置系统命令
/// @param registry 命令注册表
/// @param ctx 系统命令上下文（提供各命令所需的依赖）
void register_system_commands(CommandRegistry& registry, const SystemCommandContext& ctx);

} // namespace command
} // namespace agent
