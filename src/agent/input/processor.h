/**
 * @file processor.h
 * @brief agent/input 处理器
 */

#pragma once
#include <string>
#include <vector>
#include <optional>

#include "parser.h"
#include "types.h"
#include "agent/command/inclaude/registry.h"
#include "agent/command/inclaude/executor.h"

namespace agent::input {

class InputProcessor {
public:
    InputProcessor(std::shared_ptr<command::CommandRegistry> registry)
        : m_command_executor(std::make_shared<command::CommandExecutor>(registry)) {}

    /// 处理用户输入（异步）
    ProcessResult process(const std::string& user_input, const command::CommandContext& ctx)
    {

        ParsedInput parsed = m_parser.parse(user_input);

        switch (parsed.type) {
            case InputType::Empty:
                return {.should_query = false, .output_text = ""};

            case InputType::SlashCommand:
                return process_slash_command(*parsed.command, ctx);

            case InputType::BashCommand:
                return process_bash_command(parsed.text);

            case InputType::Text:
                return process_text_prompt(parsed);
        }
    }

private:
    ProcessResult process_slash_command(const ParsedSlashCommand& cmd, const command::CommandContext& ctx)
    {

        auto result = m_command_executor->execute("/" + cmd.command_name + " " + cmd.args, ctx);

        return {
            .should_query = result.should_query,
            .output_text = result.result.text,
            .is_error = result.result.is_error,
        };
    }

    ProcessResult process_bash_command(const std::string& command) {
        // 执行bash命令（简化实现）
        std::string output = execute_bash(command);
        return { };
    }

    ProcessResult process_text_prompt(const ParsedInput& parsed) {
        std::vector<std::string> messages;

        // 添加文本内容
        if (!parsed.text.empty()) {
            messages.push_back(parsed.text);
        }

        // 添加文件内容
        for (const auto& path : parsed.attachments) {
            std::string content = read_file_content(path);
            messages.push_back(content);
        }

        return {
            .should_query = true,
            .messages = std::move(messages),
        };
    }

    std::string execute_bash(const std::string& cmd) {
        // 实际实现应使用popen或std::process
        return "[bash] " + cmd;
    }

    std::string  read_file_content(const std::string& path){
        // 实际实现应读取文件
        return "[file content: " + path + "]";
    }

    InputParser m_parser;
    std::shared_ptr<command::CommandExecutor> m_command_executor;
};

} // namespace agent::input