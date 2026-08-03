/**
 * @file processor.h
 * @brief agent/input 处理器
 */

#pragma once
#include <string>
#include <vector>
#include <optional>
#include <fstream>
#include <filesystem>
#include <format>
#include <memory>

#include "parser.h"
#include "types.h"
#include "i_file_loader.h"
#include "agent/command/inclaude/registry.h"
#include "agent/command/inclaude/executor.h"

namespace agent::input {

class InputProcessor {
public:
    /// @brief 构造（H-11：注入 IFileLoader 隔离文件副作用）
    /// @param registry 命令注册表
    /// @param file_loader 文件加载器（H-C：显式注入，无默认实参回退，避免调用方误用）
    explicit InputProcessor(std::shared_ptr<command::CommandRegistry> registry,
                            std::shared_ptr<IFileLoader> file_loader)
        : m_command_executor(std::make_shared<command::CommandExecutor>(registry))
        , m_file_loader(std::move(file_loader)) {}

    /// 处理用户输入（异步）
    ProcessResult process(const std::string& user_input, const command::CommandContext& ctx)
    {

        ParsedInput parsed = m_parser.parse(user_input);

        switch (parsed.type) {
            case InputType::Empty:
                return {.should_query = false, .output_text = "", .messages = {}, .is_error = false};

            case InputType::SlashCommand:
                return process_slash_command(*parsed.command, ctx);

            case InputType::BashCommand:
                return process_bash_command(parsed.text);

            case InputType::Text:
                return process_text_prompt(parsed);

            default:
                return {.should_query = false, .output_text = "", .messages = {}, .is_error = false};
        }
    }

private:
    ProcessResult process_slash_command(const ParsedSlashCommand& cmd, const command::CommandContext& ctx)
    {

        auto result = m_command_executor->execute("/" + cmd.command_name + " " + cmd.args, ctx);

        return {
            .should_query = result.should_query,
            .output_text = result.result.text,
            .messages = {},
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
        std::vector<std::string> image_paths;

        // 添加文件内容（放在前面，作为上下文）
        for (const auto& path : parsed.attachments) {
            std::string content = read_file_content(path);
            messages.push_back(content);
        }

        // 图片附件：转绝对路径 + 存在性校验（失败的提示并跳过）
        for (const auto& path : parsed.image_paths) {
            std::error_code ec;
            auto abs = std::filesystem::weakly_canonical(
                std::filesystem::absolute(path, ec), ec);
            if (ec || abs.empty() || !std::filesystem::exists(abs, ec)) {
                messages.push_back(std::format("[Could not read image: {}]", path));
                continue;
            }
            image_paths.push_back(abs.string());
        }

        // 添加文本内容
        if (!parsed.text.empty()) {
            messages.push_back(parsed.text);
        }

        return {
            .should_query = true,
            .output_text = {},
            .messages = std::move(messages),
            .image_paths = std::move(image_paths),
            .is_error = false,
        };
    }

    std::string execute_bash(const std::string& cmd) {
        // 实际实现应使用popen或std::process
        return "[bash] " + cmd;
    }

    std::string read_file_content(const std::string& path) {
        // H-11：通过 IFileLoader 接口读文件，可注入 mock 测试
        // C-2：load 返回 std::optional<std::string>，区分失败与空文件
        auto content = m_file_loader->load(path);
        if (!content.has_value()) {
            // 失败：文件不存在 / 无权限 / 无法打开
            return std::format("[Could not read file: {}]", path);
        }
        // 空文件（content->empty()）属于合法文件，正常包裹返回 <file> 标签
        return std::format("<file path=\"{}\">\n{}\n</file>", path, *content);
    }

    InputParser m_parser;
    std::shared_ptr<command::CommandExecutor> m_command_executor;
    std::shared_ptr<IFileLoader> m_file_loader;  ///< H-11：文件加载器（DI 注入）
};

} // namespace agent::input