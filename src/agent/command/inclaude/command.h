/**
 * @file command.h
 * @brief Command 基类 + 三种命令类型
 * @details CommandBase 抽象基类、PromptCommand（模型执行）、LocalCommand（本地执行）
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include "types.h"

namespace agent::command {

/// 命令基类 — 对应 CommandBase
/// 所有命令类型的公共接口
class CommandBase {
public:
    virtual ~CommandBase() = default;

    /// 命令名称（用户通过 /name 调用）
    virtual const std::string& name() const { return name_; }

    /// 命令描述
    virtual const std::string& description() const { return description_; }

    /// 是否启用（动态控制）
    virtual bool is_enabled() const {
        return is_enabled_ ? is_enabled_() : true;
    }

    /// 是否隐藏（不在命令面板显示）
    virtual bool is_hidden() const { return is_hidden_; }

    /// 是否为用户可调用
    virtual bool is_user_invocable() const { return user_invocable_; }

    /// 是否禁止模型调用
    virtual bool is_model_invocation_disabled() const {
        return disable_model_invocation_;
    }

    /// 获取参数提示
    virtual const std::optional<std::string>& argument_hint() const {
        return argument_hint_;
    }

    /// 获取命令分类
    virtual const std::string& source() const { return source_; }

    /// 获取加载来源
    virtual LoadSource loaded_from() const { return loaded_from_; }

    /// 获取版本
    virtual const std::optional<std::string>& version() const {
        return version_;
    }

    /// 是否立即执行（绕过队列）
    virtual bool is_immediate() const { return immediate_; }

    /// 是否敏感命令（参数脱敏）
    virtual bool is_sensitive() const { return sensitive_; }

    /// 获取使用场景描述
    virtual const std::optional<std::string>& when_to_use() const {
        return when_to_use_;
    }

    /// 获取命令类型标识
    virtual const std::string& type() const = 0;

    // --- setters ---

    void set_is_enabled(std::function<bool()> fn) { is_enabled_ = std::move(fn); }
    void set_is_hidden(bool hidden) { is_hidden_ = hidden; }
    void set_user_invocable(bool invocable) { user_invocable_ = invocable; }
    void set_disable_model_invocation(bool disable) { disable_model_invocation_ = disable; }
    void set_argument_hint(std::string hint) { argument_hint_ = std::move(hint); }
    void set_source(std::string source) { source_ = std::move(source); }
    void set_loaded_from(LoadSource source) { loaded_from_ = source; }
    void set_version(std::string version) { version_ = std::move(version); }
    void set_immediate(bool immediate) { immediate_ = immediate; }
    void set_sensitive(bool sensitive) { sensitive_ = sensitive; }
    void set_when_to_use(std::string desc) { when_to_use_ = std::move(desc); }

protected:
    CommandBase() = default;
    CommandBase(std::string name, std::string description)
        : name_(std::move(name)), description_(std::move(description)) {}

    std::string name_;
    std::string description_;
    std::string source_{"builtin"};
    LoadSource loaded_from_{LoadSource::Builtin};

    std::optional<std::string> argument_hint_;
    std::optional<std::string> version_;
    std::optional<std::string> when_to_use_;

    std::function<bool()> is_enabled_;
    bool is_hidden_{false};
    bool user_invocable_{true};
    bool disable_model_invocation_{false};
    bool immediate_{false};
    bool sensitive_{false};
};

/// 提示词命令 — 对应 PromptCommand
/// 需要模型执行的技能命令
class PromptCommand : public CommandBase {
public:
    using PromptGenerator = std::function<std::vector<PromptBlock>(
        const std::string& args,
        const CommandContext& ctx
    )>;

    PromptCommand(std::string name, std::string description)
        : CommandBase(std::move(name), std::move(description)) {}

    const std::string& type() const override {
        static const std::string t{"prompt"};
        return t;
    }

    void set_prompt_generator(PromptGenerator gen) {
        prompt_generator_ = std::move(gen);
    }

    /// 生成提示词内容
    std::vector<PromptBlock> generate_prompt(const std::string& args, const CommandContext& ctx) const {
        if (!prompt_generator_) return {};
        return prompt_generator_(args, ctx);
    }

    void set_progress_message(std::string msg) { progress_message_ = std::move(msg); }
    const std::string& progress_message() const { return progress_message_; }

    void set_content_length(size_t len) { content_length_ = len; }
    size_t content_length() const { return content_length_; }

    void set_allowed_tools(std::vector<std::string> tools) { allowed_tools_ = std::move(tools); }
    const std::vector<std::string>& allowed_tools() const { return allowed_tools_; }

    void set_arg_names(std::vector<std::string> names) { arg_names_ = std::move(names); }
    const std::vector<std::string>& arg_names() const { return arg_names_; }

    void set_model(std::string model) { model_ = std::move(model); }
    const std::optional<std::string>& model() const { return model_; }

    void set_context_type(std::string ctx) { context_type_ = std::move(ctx); }
    const std::optional<std::string>& context_type() const { return context_type_; }

private:
    PromptGenerator prompt_generator_;
    std::string progress_message_;
    size_t content_length_{0};
    std::vector<std::string> allowed_tools_;
    std::vector<std::string> arg_names_;
    std::optional<std::string> context_type_;
    std::optional<std::string> model_;
};

/// 本地命令 — 对应 LocalCommand
/// 纯文本输出的本地命令，无 UI
class LocalCommand : public CommandBase {
public:
    using CommandCall = std::function<CommandResult(
        const std::string& args,
        const CommandContext& ctx
    )>;

    LocalCommand(std::string name, std::string description)
        : CommandBase(std::move(name), std::move(description)) {}

    const std::string& type() const override {
        static const std::string t{"local"};
        return t;
    }

    void set_call(CommandCall call) { call_ = std::move(call); }

    /// 执行命令
    CommandResult call(const std::string& args, const CommandContext& ctx) const {
        if (!call_) return CommandResult::error("Command not implemented");
        return call_(args, ctx);
    }

    void set_supports_non_interactive(bool supported) { supports_non_interactive_ = supported; }
    bool supports_non_interactive() const { return supports_non_interactive_; }

private:
    CommandCall call_;
    bool supports_non_interactive_{true};
};

/// 便捷函数：创建 PromptCommand
inline std::shared_ptr<PromptCommand> make_prompt_command(std::string name, std::string description) {
    return std::make_shared<PromptCommand>(std::move(name), std::move(description));
}

/// 便捷函数：创建 LocalCommand
inline std::shared_ptr<LocalCommand> make_local_command(std::string name, std::string description) {
    return std::make_shared<LocalCommand>(std::move(name), std::move(description));
}

} // namespace agent::command
