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

#include "agent/command/registry.h"

namespace workx {

class ChatSession;

namespace command {

/// 系统命令注册所需的上下文
/// 各命令通过 lambda 捕获所需的依赖
struct SystemCommandContext {
    ChatSession* session = nullptr;       ///< 会话指针（clear/regen 需要）
    std::function<void()> on_exit;        ///< 退出回调（exit/quit 触发）
    std::function<void()> on_model_select; ///< 模型选择回调（model 触发）
};

/// 注册内置系统命令
/// @param registry 命令注册表
/// @param ctx 系统命令上下文（提供各命令所需的依赖）
void register_system_commands(CommandRegistry& registry, const SystemCommandContext& ctx);

} // namespace command
} // namespace workx
