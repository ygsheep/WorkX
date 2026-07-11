# Agent Core — C++ 命令系统实现

> 基于 Claude Code 源码的 `Command` 类型（commands.ts + types/command.ts）映射到现代 C++20。
> 保留命令面板核心功能，剥离 React/JSX/TUI 相关层，仅保留必要的命令类型和属性。

---

## 一、设计映射原则

| Claude Code (TS) | C++ 最小实现 | 映射策略 |
|---|---|---|
| `CommandBase` 接口 | `CommandBase` 抽象类 | 纯虚接口，子类继承 |
| `PromptCommand` / `LocalCommand` / `LocalJSXCommand` | 三个具体子类 | 继承体系，而非变体 |
| 懒加载 `load()` | `std::function` 工厂 | 延迟加载命令实现 |
| 回调函数 `isEnabled()` | `std::function<bool()>` | 函数对象替代方法 |
| React JSX | ❌ 不映射 | 最小实现不需要 UI |
| MCP 相关 | ❌ 不映射 | 后续扩展点 |
| `getPromptForCommand()` | 协程返回 JSON | 返回结构化提示词内容 |

---

## 二、目录结构

在现有项目结构中新增：

```

│   └── agent/
│       └── command/
│           ├── command.hpp/.cpp         # ⭐ Command 基类 + 三种命令类型
│           ├── registry.hpp/.cpp        # ⭐ 命令注册表
│           ├── types.hpp           # 命令相关类型定义
│           └── executor.hpp/.cpp        # 命令执行器（处理用户输入分发）
│
```

---

## 三、核心类型定义

### 3.1 `types.hpp` — 命令相关类型

```cpp
#pragma once
#include <string>
#include <vector>
#include <optional>
#include <nlohmann/json.hpp>
#include "agent/tool/context.hpp"

namespace agent::command {

/// 命令可用性 — 对应 CommandAvailability
enum class Availability {
    Universal,    /// 无限制，默认
    ClaudeAI,     /// 仅 claude.ai 订阅用户
    Console,      /// 仅 Console API key 用户
};

/// 命令加载来源 — 对应 loadedFrom
enum class LoadSource {
    Builtin,
    Skills,
    Plugin,
    Bundled,
    MCP,
    Deprecated,
};

/// 命令执行上下文 — 复用 ToolContext，扩展命令特有字段
/// 对应精简版 LocalJSXCommandContext
struct CommandContext {
    tool::ToolContext tool_context;  /// 复用工具执行上下文
    std::string model;               /// 当前模型名称
    nlohmann::json options;          /// 额外选项
};

/// 命令结果 — 对应 LocalCommandResult
struct CommandResult {
    enum class Type {
        Text,       /// 文本输出
        Compact,    /// 压缩结果
        Skip,       /// 跳过消息
    };
    
    Type type{Type::Text};
    std::string text;              /// 文本输出内容
    nlohmann::json compact_data;   /// 压缩结果数据（可选）
    bool is_error{false};          /// 是否错误
    
    static auto ok(std::string text) -> CommandResult {
        return {.type = Type::Text, .text = std::move(text)};
    }
    
    static auto error(std::string msg) -> CommandResult {
        return {.type = Type::Text, .text = std::move(msg), .is_error = true};
    }
};

/// 提示词内容块 — 对应 ContentBlockParam
struct PromptBlock {
    std::string type;           /// "text" | "image" | "tool_result"
    std::string text;           /// 文本内容
    nlohmann::json image;       /// 图片内容（可选）
};

} // namespace agent::command
```

### 3.2 `command.hpp` — 命令基类与三种类型

```cpp
#pragma once
#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <cppcoro/task.hpp>
#include "agent/command/types.hpp"

namespace agent::command {

/// 命令基类 — 对应 CommandBase
/// 所有命令类型的公共接口
class CommandBase {
public:
    virtual ~CommandBase() = default;

    /// 命令名称（用户通过 /name 调用）
    virtual auto name() const -> const std::string& = 0;

    /// 命令描述
    virtual auto description() const -> const std::string& = 0;

    /// 是否启用（动态控制）
    virtual auto is_enabled() const -> bool {
        return is_enabled_ ? is_enabled_() : true;
    }

    /// 是否隐藏（不在命令面板显示）
    virtual auto is_hidden() const -> bool {
        return is_hidden_;
    }

    /// 是否为用户可调用
    virtual auto is_user_invocable() const -> bool {
        return user_invocable_;
    }

    /// 是否禁止模型调用
    virtual auto is_model_invocation_disabled() const -> bool {
        return disable_model_invocation_;
    }

    /// 获取参数提示
    virtual auto argument_hint() const -> const std::optional<std::string>& {
        return argument_hint_;
    }

    /// 获取命令分类
    virtual auto source() const -> const std::string& {
        return source_;
    }

    /// 获取加载来源
    virtual auto loaded_from() const -> LoadSource {
        return loaded_from_;
    }

    /// 获取版本
    virtual auto version() const -> const std::optional<std::string>& {
        return version_;
    }

    /// 是否立即执行（绕过队列）
    virtual auto is_immediate() const -> bool {
        return immediate_;
    }

    /// 是否敏感命令（参数脱敏）
    virtual auto is_sensitive() const -> bool {
        return sensitive_;
    }

    /// 获取使用场景描述
    virtual auto when_to_use() const -> const std::optional<std::string>& {
        return when_to_use_;
    }

    /// 获取命令类型标识
    virtual auto type() const -> const std::string& = 0;

    /// 设置启用状态回调
    auto set_is_enabled(std::function<bool()> fn) -> void {
        is_enabled_ = std::move(fn);
    }

    /// 设置是否隐藏
    auto set_is_hidden(bool hidden) -> void {
        is_hidden_ = hidden;
    }

    /// 设置是否用户可调用
    auto set_user_invocable(bool invocable) -> void {
        user_invocable_ = invocable;
    }

    /// 设置是否禁止模型调用
    auto set_disable_model_invocation(bool disable) -> void {
        disable_model_invocation_ = disable;
    }

    /// 设置参数提示
    auto set_argument_hint(std::string hint) -> void {
        argument_hint_ = std::move(hint);
    }

    /// 设置命令分类
    auto set_source(std::string source) -> void {
        source_ = std::move(source);
    }

    /// 设置加载来源
    auto set_loaded_from(LoadSource source) -> void {
        loaded_from_ = source;
    }

    /// 设置版本
    auto set_version(std::string version) -> void {
        version_ = std::move(version);
    }

    /// 设置是否立即执行
    auto set_immediate(bool immediate) -> void {
        immediate_ = immediate;
    }

    /// 设置是否敏感命令
    auto set_sensitive(bool sensitive) -> void {
        sensitive_ = sensitive;
    }

    /// 设置使用场景描述
    auto set_when_to_use(std::string desc) -> void {
        when_to_use_ = std::move(desc);
    }

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
    using PromptGenerator = std::function<cppcoro::task<std::vector<PromptBlock>>(
        const std::string& args,
        const CommandContext& ctx
    )>;

    PromptCommand(std::string name, std::string description)
        : CommandBase(std::move(name), std::move(description)) {}

    auto type() const -> const std::string& override {
        static const std::string type{"prompt"};
        return type;
    }

    /// 设置提示词生成函数（异步）
    auto set_prompt_generator(PromptGenerator gen) -> void {
        prompt_generator_ = std::move(gen);
    }

    /// 生成提示词内容（异步）
    auto generate_prompt(const std::string& args, const CommandContext& ctx)
        -> cppcoro::task<std::vector<PromptBlock>> {
        if (!prompt_generator_) {
            co_return {};
        }
        co_return co_await prompt_generator_(args, ctx);
    }

    /// 设置进度消息
    auto set_progress_message(std::string msg) -> void {
        progress_message_ = std::move(msg);
    }

    /// 获取进度消息
    auto progress_message() const -> const std::string& {
        return progress_message_;
    }

    /// 设置内容长度（用于 Token 估算）
    auto set_content_length(size_t len) -> void {
        content_length_ = len;
    }

    /// 获取内容长度
    auto content_length() const -> size_t {
        return content_length_;
    }

    /// 设置允许的工具列表
    auto set_allowed_tools(std::vector<std::string> tools) -> void {
        allowed_tools_ = std::move(tools);
    }

    /// 获取允许的工具列表
    auto allowed_tools() const -> const std::vector<std::string>& {
        return allowed_tools_;
    }

    /// 设置执行上下文类型（inline | fork）
    auto set_context_type(std::string ctx) -> void {
        context_type_ = std::move(ctx);
    }

    /// 获取执行上下文类型
    auto context_type() const -> const std::optional<std::string>& {
        return context_type_;
    }

    /// 设置参数名称列表
    auto set_arg_names(std::vector<std::string> names) -> void {
        arg_names_ = std::move(names);
    }

    /// 获取参数名称列表
    auto arg_names() const -> const std::vector<std::string>& {
        return arg_names_;
    }

    /// 设置模型名称
    auto set_model(std::string model) -> void {
        model_ = std::move(model);
    }

    /// 获取模型名称
    auto model() const -> const std::optional<std::string>& {
        return model_;
    }

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
    using CommandCall = std::function<cppcoro::task<CommandResult>(
        const std::string& args,
        const CommandContext& ctx
    )>;

    LocalCommand(std::string name, std::string description)
        : CommandBase(std::move(name), std::move(description)) {}

    auto type() const -> const std::string& override {
        static const std::string type{"local"};
        return type;
    }

    /// 设置命令调用函数（异步）
    auto set_call(CommandCall call) -> void {
        call_ = std::move(call);
    }

    /// 执行命令（异步）
    auto call(const std::string& args, const CommandContext& ctx)
        -> cppcoro::task<CommandResult> {
        if (!call_) {
            co_return CommandResult::error("Command not implemented");
        }
        co_return co_await call_(args, ctx);
    }

    /// 设置是否支持非交互式模式
    auto set_supports_non_interactive(bool supported) -> void {
        supports_non_interactive_ = supported;
    }

    /// 是否支持非交互式模式
    auto supports_non_interactive() const -> bool {
        return supports_non_interactive_;
    }

private:
    CommandCall call_;
    bool supports_non_interactive_{true};
};

/// 便捷函数：创建 PromptCommand
inline auto make_prompt_command(std::string name, std::string description)
    -> std::shared_ptr<PromptCommand> {
    return std::make_shared<PromptCommand>(std::move(name), std::move(description));
}

/// 便捷函数：创建 LocalCommand
inline auto make_local_command(std::string name, std::string description)
    -> std::shared_ptr<LocalCommand> {
    return std::make_shared<LocalCommand>(std::move(name), std::move(description));
}

} // namespace agent::command
```

### 3.3 `registry.hpp` — 命令注册表

```cpp
#pragma once
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include "agent/command/command.hpp"

namespace agent::command {

/// 命令注册表 — 对应 commands.ts 的 getCommands() / getCommand()
///
/// 管理所有可用命令，支持按名查找和分类过滤。
class CommandRegistry {
public:
    /// 注册一个命令
    auto register_command(std::shared_ptr<CommandBase> cmd) -> void;

    /// 按名查找命令（精确匹配）
    auto find_by_name(const std::string& name) const
        -> std::shared_ptr<CommandBase>;

    /// 获取所有已启用命令
    auto get_enabled_commands() const
        -> std::vector<std::shared_ptr<CommandBase>>;

    /// 获取所有用户可调用命令（用于命令面板）
    auto get_user_invocable_commands() const
        -> std::vector<std::shared_ptr<CommandBase>>;

    /// 获取所有模型可调用命令（用于 SkillTool）
    auto get_model_invocable_commands() const
        -> std::vector<std::shared_ptr<CommandBase>>;

    /// 获取指定类型的命令
    auto get_by_type(const std::string& type) const
        -> std::vector<std::shared_ptr<CommandBase>>;

    /// 获取指定来源的命令
    auto get_by_source(LoadSource source) const
        -> std::vector<std::shared_ptr<CommandBase>>;

    /// 检查命令是否存在
    auto exists(const std::string& name) const -> bool;

    /// 获取命令总数
    auto size() const -> size_t;

private:
    std::vector<std::shared_ptr<CommandBase>> commands_;
    std::unordered_map<std::string, std::shared_ptr<CommandBase>> name_index_;
};

} // namespace agent::command
```

### 3.4 `executor.hpp` — 命令执行器

```cpp
#pragma once
#include <string>
#include <memory>
#include <nlohmann/json.hpp>
#include <cppcoro/task.hpp>
#include "agent/command/command.hpp"
#include "agent/command/registry.hpp"
#include "agent/command/types.hpp"

namespace agent::command {

/// 命令执行结果（扩展版，包含元信息）
struct ExecutorResult {
    CommandResult result;
    std::string command_name;
    bool should_query{false};       /// 是否需要后续查询模型
    std::optional<std::string> next_input;
    bool submit_next_input{false};
};

/// 命令执行器 — 对应 processSlashCommand.tsx 的核心逻辑
///
/// 负责解析用户输入、查找命令、执行命令并返回结果。
class CommandExecutor {
public:
    explicit CommandExecutor(std::shared_ptr<CommandRegistry> registry);

    /// 解析并执行命令（异步）
    /// input: 用户输入（如 "/help" 或 "/init project"）
    auto execute(const std::string& input, const CommandContext& ctx)
        -> cppcoro::task<ExecutorResult>;

    /// 解析命令名称和参数
    /// 返回: (command_name, args)
    auto parse(const std::string& input) -> std::pair<std::string, std::string>;

private:
    std::shared_ptr<CommandRegistry> registry_;
};

} // namespace agent::command
```

---

## 四、实现文件

### 4.1 `registry.cpp` — 注册表实现

```cpp
#include "agent/command/registry.hpp"

namespace agent::command {

auto CommandRegistry::register_command(std::shared_ptr<CommandBase> cmd) -> void {
    commands_.push_back(cmd);
    name_index_[cmd->name()] = cmd;
}

auto CommandRegistry::find_by_name(const std::string& name) const
    -> std::shared_ptr<CommandBase> {
    auto it = name_index_.find(name);
    return it != name_index_.end() ? it->second : nullptr;
}

auto CommandRegistry::get_enabled_commands() const
    -> std::vector<std::shared_ptr<CommandBase>> {
    std::vector<std::shared_ptr<CommandBase>> result;
    for (auto& cmd : commands_) {
        if (cmd->is_enabled()) {
            result.push_back(cmd);
        }
    }
    return result;
}

auto CommandRegistry::get_user_invocable_commands() const
    -> std::vector<std::shared_ptr<CommandBase>> {
    std::vector<std::shared_ptr<CommandBase>> result;
    for (auto& cmd : commands_) {
        if (cmd->is_enabled() && cmd->is_user_invocable() && !cmd->is_hidden()) {
            result.push_back(cmd);
        }
    }
    return result;
}

auto CommandRegistry::get_model_invocable_commands() const
    -> std::vector<std::shared_ptr<CommandBase>> {
    std::vector<std::shared_ptr<CommandBase>> result;
    for (auto& cmd : commands_) {
        if (cmd->is_enabled() && !cmd->is_model_invocation_disabled()) {
            if (dynamic_cast<PromptCommand*>(cmd.get())) {
                result.push_back(cmd);
            }
        }
    }
    return result;
}

auto CommandRegistry::get_by_type(const std::string& type) const
    -> std::vector<std::shared_ptr<CommandBase>> {
    std::vector<std::shared_ptr<CommandBase>> result;
    for (auto& cmd : commands_) {
        if (cmd->type() == type) {
            result.push_back(cmd);
        }
    }
    return result;
}

auto CommandRegistry::get_by_source(LoadSource source) const
    -> std::vector<std::shared_ptr<CommandBase>> {
    std::vector<std::shared_ptr<CommandBase>> result;
    for (auto& cmd : commands_) {
        if (cmd->loaded_from() == source) {
            result.push_back(cmd);
        }
    }
    return result;
}

auto CommandRegistry::exists(const std::string& name) const -> bool {
    return name_index_.contains(name);
}

auto CommandRegistry::size() const -> size_t {
    return commands_.size();
}

} // namespace agent::command
```

### 4.2 `executor.cpp` — 执行器实现

```cpp
#include "agent/command/executor.hpp"

namespace agent::command {

CommandExecutor::CommandExecutor(std::shared_ptr<CommandRegistry> registry)
    : registry_(std::move(registry)) {}

auto CommandExecutor::execute(const std::string& input, const CommandContext& ctx)
    -> cppcoro::task<ExecutorResult> {
    auto [command_name, args] = parse(input);
    
    auto cmd = registry_->find_by_name(command_name);
    if (!cmd) {
        co_return ExecutorResult{
            .result = CommandResult::error("Command not found: " + command_name),
            .command_name = command_name,
        };
    }

    if (!cmd->is_enabled()) {
        co_return ExecutorResult{
            .result = CommandResult::error("Command is disabled: " + command_name),
            .command_name = command_name,
        };
    }

    if (cmd->is_sensitive()) {
        // 敏感命令：记录日志但不记录参数详情
    }

    CommandResult result;
    bool should_query = false;

    if (auto* local_cmd = dynamic_cast<LocalCommand*>(cmd.get())) {
        result = co_await local_cmd->call(args, ctx);
    } else if (auto* prompt_cmd = dynamic_cast<PromptCommand*>(cmd.get())) {
        auto blocks = co_await prompt_cmd->generate_prompt(args, ctx);
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

    co_return ExecutorResult{
        .result = std::move(result),
        .command_name = command_name,
        .should_query = should_query,
    };
}

auto CommandExecutor::parse(const std::string& input)
    -> std::pair<std::string, std::string> {
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
```

---

## 五、内置命令示例

### 5.1 `help` 命令（LocalCommand）

```cpp
#include "agent/command/command.hpp"
#include "agent/command/registry.hpp"

auto create_help_command(std::shared_ptr<CommandRegistry> registry)
    -> std::shared_ptr<LocalCommand> {
    auto cmd = make_local_command("help", "Show available commands");
    
    cmd->set_call([registry](const std::string&, const CommandContext&) -> cppcoro::task<CommandResult> {
        auto cmds = registry->get_user_invocable_commands();
        std::string result = "Available commands:\n";
        for (auto& c : cmds) {
            result += "  /" + c->name() + " - " + c->description() + "\n";
        }
        co_return CommandResult::ok(result);
    });
    
    return cmd;
}
```

### 5.2 `init` 命令（PromptCommand）

```cpp
#include "agent/command/command.hpp"

auto create_init_command() -> std::shared_ptr<PromptCommand> {
    auto cmd = make_prompt_command("init", "Initialize project configuration");
    
    cmd->set_progress_message("analyzing your codebase");
    cmd->set_content_length(1000);
    
    cmd->set_prompt_generator([](const std::string& args, const CommandContext& ctx) -> cppcoro::task<std::vector<PromptBlock>> {
        std::string prompt = R"(
You are an AI assistant helping to initialize a project.
Current directory: {{cwd}}
Arguments: {{args}}

Please analyze the project structure and suggest a CLAUDE.md configuration.
)";
        
        size_t pos = prompt.find("{{cwd}}");
        if (pos != std::string::npos) {
            prompt.replace(pos, 6, ctx.tool_context.cwd);
        }
        pos = prompt.find("{{args}}");
        if (pos != std::string::npos) {
            prompt.replace(pos, 7, args);
        }
        
        co_return std::vector<PromptBlock>{{
            .type = "text",
            .text = std::move(prompt),
        }};
    });
    
    return cmd;
}
```

---

## 六、集成到 QueryEngine

在 `query_engine.cpp` 中集成命令系统：

```cpp
#include "agent/command/registry.hpp"
#include "agent/command/executor.hpp"

// 在 QueryEngine 类中添加成员：
class QueryEngine {
    // ... 原有成员 ...
private:
    std::shared_ptr<command::CommandRegistry> command_registry_;
    std::shared_ptr<command::CommandExecutor> command_executor_;
};

// 在构造函数中：
QueryEngine::QueryEngine(QueryEngineConfig config) {
    // ... 原有初始化 ...
    
    command_registry_ = std::make_shared<command::CommandRegistry>();
    command_registry_->register_command(create_help_command(command_registry_));
    command_registry_->register_command(create_init_command());
    
    command_executor_ = std::make_shared<command::CommandExecutor>(command_registry_);
}

// 在 submit() 方法中：
auto QueryEngine::submit(std::string user_input)
    -> cppcoro::async_generator<message::OutputEvent> {
    
    if (!user_input.empty() && user_input[0] == '/') {
        command::CommandContext cmd_ctx{
            .tool_context = tool_context_,
            .model = config_.model,
        };
        
        auto result = co_await command_executor_->execute(user_input, cmd_ctx);
        
        co_yield message::OutputEvent{
            .type = result.result.is_error 
                ? message::OutputEvent::Error 
                : message::OutputEvent::Text,
            .text = result.result.text,
            .is_error = result.result.is_error,
        };
        
        if (result.should_query && !result.result.is_error) {
            // 将命令结果作为系统提示的一部分，继续 ReAct 循环
        }
        
        co_return;
    }
    
    // ... 原有逻辑 ...
}
```

---

## 七、与 Claude Code 的映射对照表

| Claude Code 属性 | C++ 属性 | 精简说明 |
|---|---|---|
| `name` | `name_` | 保留 |
| `description` | `description_` | 保留 |
| `isEnabled` | `is_enabled_` (std::function) | 保留，用函数对象 |
| `isHidden` | `is_hidden_` | 保留 |
| `aliases` | ❌ 不保留 | 最小实现不需要，后续可加 |
| `argumentHint` | `argument_hint_` | 保留 |
| `whenToUse` | `when_to_use_` | 保留 |
| `version` | `version_` | 保留 |
| `disableModelInvocation` | `disable_model_invocation_` | 保留 |
| `userInvocable` | `user_invocable_` | 保留 |
| `loadedFrom` | `loaded_from_` (LoadSource) | 保留，枚举化 |
| `kind` | ❌ 不保留 | 工作流后续扩展 |
| `immediate` | `immediate_` | 保留 |
| `isSensitive` | `sensitive_` | 保留 |
| `userFacingName` | ❌ 不保留 | 最小实现不需要 |

| Claude Code 命令类型 | C++ 类 | 精简说明 |
|---|---|---|
| `PromptCommand` | `PromptCommand` | 保留核心：generate_prompt（异步） |
| `LocalCommand` | `LocalCommand` | 保留核心：call（异步） |
| `LocalJSXCommand` | ❌ 不保留 | 无 UI 层 |

---

## 八、扩展路径

```
Phase 0 (当前): CommandBase + PromptCommand + LocalCommand + Registry + Executor
    │
    ├─ Phase 1: 别名支持 (aliases)
    │     修改: CommandBase 添加 aliases_，Registry 添加 alias_index_
    │
    ├─ Phase 2: 权限控制 (availability)
    │     修改: CommandBase 添加 availability_，Registry 过滤逻辑
    │
    ├─ Phase 3: MCP 命令支持
    │     新增: McpCommand 类
    │     修改: Registry 合并 MCP 命令
    │
    └─ Phase 4: 工作流命令 (Workflow)
          新增: WorkflowCommand 类
          修改: Executor 支持工作流调度
```
