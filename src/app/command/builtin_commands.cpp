/**
 * @file builtin_commands.cpp
 * @brief 内置系统命令注册实现
 * @details 注册 help/exit/quit/clear/regen/model 系统命令到 CommandRegistry，
 *          各命令通过 lambda 捕获所需依赖（ChatSession*、回调等）
 * @version 1.0.0
 * @date 2026-07
 */

#include <cctype>
#include <format>

#include "agent/command/inclaude/command.h"
#include "agent/core/chat_session.h"
#include "app/command/builtin_commands.h"

namespace agent::command {

void register_system_commands(CommandRegistry& registry, const SystemCommandContext& ctx) {
    CommandRegistry* reg_ptr = &registry;
    std::unique_ptr<ChatSession>* session_ref = ctx.session;
    auto on_exit = ctx.on_exit;
    auto on_model_select = ctx.on_model_select;
    auto on_provider_select = ctx.on_provider_select;
    auto on_resume = ctx.on_resume;

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
    clear_cmd->set_call([session_ref](const std::string& /*args*/, const CommandContext& /*c*/) -> CommandResult {
        if (!session_ref || !*session_ref) return CommandResult::error("No active session");
        (*session_ref)->clear_history();
        return CommandResult::ok("Session history cleared.\n");
    });
    registry.register_command(clear_cmd);

    // regen: 重新生成上一条回复
    auto regen_cmd = make_local_command("regen", "重新生成上一条回复");
    regen_cmd->set_argument_hint("/regen");
    regen_cmd->set_call([session_ref](const std::string& /*args*/, const CommandContext& /*c*/) -> CommandResult {
        if (!session_ref || !*session_ref) return CommandResult::error("No active session");
        (*session_ref)->regenerate();
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

    // provider: 交互式切换 API 供应商
    auto provider_cmd = make_local_command("provider", "切换 API 供应商");
    provider_cmd->set_argument_hint("/provider");
    provider_cmd->set_call([on_provider_select](const std::string& /*args*/, const CommandContext& /*c*/) -> CommandResult {
        if (on_provider_select) on_provider_select();
        return CommandResult::ok("");
    });
    registry.register_command(provider_cmd);

    // resume: 切换到历史会话
    auto resume_cmd = make_local_command("resume", "切换到历史会话");
    resume_cmd->set_argument_hint("/resume");
    resume_cmd->set_call([on_resume](const std::string& /*args*/, const CommandContext& /*c*/) -> CommandResult {
        if (on_resume) on_resume();
        return CommandResult::ok("");
    });
    registry.register_command(resume_cmd);

    // rename: 修改当前会话标题
    auto rename_cmd = make_local_command("rename", "修改当前会话标题");
    rename_cmd->set_argument_hint("/rename <title>");
    rename_cmd->set_call([session_ref](const std::string& args, const CommandContext& /*c*/) -> CommandResult {
        if (!session_ref || !*session_ref) return CommandResult::error("No active session");
        std::string title = args;
        // 去除首尾空白
        while (!title.empty() && std::isspace(static_cast<unsigned char>(title.front()))) title.erase(0, 1);
        while (!title.empty() && std::isspace(static_cast<unsigned char>(title.back()))) title.pop_back();
        if (title.empty()) {
            return CommandResult::error("标题不能为空，用法：/rename <title>");
        }
        if ((*session_ref)->rename_session(title)) {
            return CommandResult::ok(std::format("会话标题已修改为：{}\n", title));
        }
        return CommandResult::error("修改标题失败（会话未持久化？）");
    });
    registry.register_command(rename_cmd);
}

} // namespace agent::command
