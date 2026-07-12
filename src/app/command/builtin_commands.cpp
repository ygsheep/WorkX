/**
 * @file builtin_commands.cpp
 * @brief 内置系统命令注册实现
 * @details 注册 help/exit/quit/clear/regen/model 系统命令到 CommandRegistry，
 *          各命令通过 lambda 捕获所需依赖（ChatSession*、回调等）
 * @version 1.0.0
 * @date 2026-07
 */

#include <format>

#include "../../agent/command/inclaude/command.h"
#include "agent/core/chat_session.h"
#include "app/command/builtin_commands.h"

namespace agent::command {

void register_system_commands(CommandRegistry& registry, const SystemCommandContext& ctx) {
    CommandRegistry* reg_ptr = &registry;
    ChatSession* session = ctx.session;
    auto on_exit = ctx.on_exit;
    auto on_model_select = ctx.on_model_select;

    // help: 列出所有可用命令
    auto help_cmd = make_local_command("help", "显示可用命令列表");
    help_cmd->set_argument_hint("/help");
    help_cmd->set_call([reg_ptr](const std::string& /*args*/, const CommandContext& /*c*/) -> CommandResult {
        std::string output = "Available commands:\n";
        for (const auto& cmd : reg_ptr->get_user_invocable_commands()) {
            output += std::format("  /{:<10} - {}\n", cmd->name(), cmd->description());
        }
        return CommandResult::ok(std::move(output));
    });
    registry.register_command(help_cmd);

    // exit: 退出程序
    auto exit_cmd = make_local_command("exit", "退出程序");
    exit_cmd->set_argument_hint("/exit");
    exit_cmd->set_call([on_exit](const std::string& /*args*/, const CommandContext& /*c*/) -> CommandResult {
        if (on_exit) on_exit();
        return CommandResult::ok("");
    });
    registry.register_command(exit_cmd);

    // quit: 退出程序（别名）
    auto quit_cmd = make_local_command("quit", "退出程序（别名）");
    quit_cmd->set_argument_hint("/quit");
    quit_cmd->set_call([on_exit](const std::string& /*args*/, const CommandContext& /*c*/) -> CommandResult {
        if (on_exit) on_exit();
        return CommandResult::ok("");
    });
    registry.register_command(quit_cmd);

    // clear: 清除会话历史
    auto clear_cmd = make_local_command("clear", "清除会话历史");
    clear_cmd->set_argument_hint("/clear");
    clear_cmd->set_call([session](const std::string& /*args*/, const CommandContext& /*c*/) -> CommandResult {
        if (!session) return CommandResult::error("No active session");
        session->clear_history();
        return CommandResult::ok("Session history cleared.\n");
    });
    registry.register_command(clear_cmd);

    // regen: 重新生成上一条回复
    auto regen_cmd = make_local_command("regen", "重新生成上一条回复");
    regen_cmd->set_argument_hint("/regen");
    regen_cmd->set_call([session](const std::string& /*args*/, const CommandContext& /*c*/) -> CommandResult {
        if (!session) return CommandResult::error("No active session");
        session->regenerate();
        return CommandResult::ok("");
    });
    registry.register_command(regen_cmd);

    // model: 交互式选择 AI 模型
    auto model_cmd = make_local_command("model", "交互式选择 AI 模型");
    model_cmd->set_argument_hint("/model");
    model_cmd->set_call([on_model_select](const std::string& /*args*/, const CommandContext& /*c*/) -> CommandResult {
        if (on_model_select) on_model_select();
        return CommandResult::ok("");
    });
    registry.register_command(model_cmd);
}

} // namespace workx::command
