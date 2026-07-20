/**
 * @file executor.cpp
 * @brief 命令执行器实现
 * @details 命令解析与分发执行的实现
 * @version 1.0.0
 * @date 2026-07
 */

#include "agent/command/inclaude/executor.h"

namespace agent::command {

CommandExecutor::CommandExecutor(std::shared_ptr<CommandRegistry> registry)
    : registry_(std::move(registry)) {}

ExecutorResult CommandExecutor::execute(const std::string& input, const CommandContext& ctx) const {
    auto [command_name, args] = parse(input);

    auto cmd = registry_->find_by_name(command_name);
    if (!cmd) {
        return {
            .result = CommandResult::error("Command not found: " + command_name),
            .command_name = command_name,
        };
    }

    if (!cmd->is_enabled()) {
        return {
            .result = CommandResult::error("Command is disabled: " + command_name),
            .command_name = command_name,
        };
    }

    // 敏感命令：日志脱敏（后续扩展点）

    CommandResult result;
    bool should_query = false;
    // TODO: CommandResult::Type::Compact not yet handled (depends on agent/compact/ stub).
    //       When a local command returns Compact, the result is currently passed through
    //       unchanged; add explicit Compact dispatch here once compact/ is implemented.

    if (auto* local_cmd = dynamic_cast<LocalCommand*>(cmd.get())) {
        result = local_cmd->call(args, ctx);
    } else if (auto* prompt_cmd = dynamic_cast<PromptCommand*>(cmd.get())) {
        auto blocks = prompt_cmd->generate_prompt(args, ctx);
        std::string prompt_text;
        for (auto& block : blocks) {
            if (!prompt_text.empty()) prompt_text += "\n";
            prompt_text += block.text;
        }
        result = CommandResult::ok(std::move(prompt_text));
        should_query = true;
    } else {
        result = CommandResult::error("Unknown command type");
    }

    return {
        .result = std::move(result),
        .command_name = command_name,
        .should_query = should_query,
    };
}

std::pair<std::string, std::string> CommandExecutor::parse(const std::string& input) {
    if (input.empty()) {
        return {"", ""};
    }

    size_t start = 0;
    if (input[0] == '/') {
        start = 1;
    }

    size_t space_pos = input.find(' ', start);
    if (space_pos == std::string::npos) {
        return {input.substr(start), ""};
    }

    std::string command_name = input.substr(start, space_pos - start);
    std::string args = input.substr(space_pos + 1);
    return {command_name, args};
}

} // namespace agent::command
