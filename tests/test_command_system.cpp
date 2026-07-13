/**
 * @file test_command_system.cpp
 * @brief CommandRegistry + CommandExecutor + 内置命令单元测试
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include "../src/agent/command/inclaude/registry.h"
#include "agent/command/inclaude/executor.h"
#include "app/command/builtin_commands.h"
#include "../src/agent/command/inclaude/command.h"
#include "agent/core/chat_session.h"
#include "agent/api/i_backend.h"
#include "core/config/config_manager.h"

using namespace agent;
using namespace agent::command;

/// @brief Mock ICompletionProvider 用于测试
class MockProvider : public ICompletionProvider {
public:
    std::unique_ptr<IStreamReader> submit_completion(const CompletionRequest&) override {
        return nullptr;
    }
    void interrupt() override {}
    bool is_generating() const override { return false; }
};

/// @brief 创建测试用 ChatSession
static std::unique_ptr<ChatSession> make_test_session() {
    return std::make_unique<ChatSession>(
        std::unique_ptr<ICompletionProvider>(new MockProvider())
    );
}

// ============================================================
// CommandRegistry 测试
// ============================================================

TEST_CASE("CommandRegistry register and find", "[command]") {
    CommandRegistry registry;

    auto cmd = make_local_command("test", "test command");
    registry.register_command(cmd);

    REQUIRE(registry.size() == 1);
    REQUIRE(registry.exists("test"));
    REQUIRE_FALSE(registry.exists("nonexistent"));

    auto found = registry.find_by_name("test");
    REQUIRE(found != nullptr);
    REQUIRE(found->name() == "test");
    REQUIRE(found->description() == "test command");
}

TEST_CASE("CommandRegistry get_user_invocable_commands", "[command]") {
    CommandRegistry registry;

    auto cmd1 = make_local_command("visible", "visible command");
    auto cmd2 = make_local_command("hidden", "hidden command");
    cmd2->set_is_hidden(true);
    auto cmd3 = make_local_command("disabled", "disabled command");
    cmd3->set_is_enabled([]() { return false; });

    registry.register_command(cmd1);
    registry.register_command(cmd2);
    registry.register_command(cmd3);

    auto invocable = registry.get_user_invocable_commands();
    REQUIRE(invocable.size() == 1);
    REQUIRE(invocable[0]->name() == "visible");
}

TEST_CASE("CommandRegistry get_by_type", "[command]") {
    CommandRegistry registry;

    auto local_cmd = make_local_command("local_cmd", "local");
    auto prompt_cmd = make_prompt_command("prompt_cmd", "prompt");

    registry.register_command(local_cmd);
    registry.register_command(prompt_cmd);

    auto locals = registry.get_by_type("local");
    REQUIRE(locals.size() == 1);
    REQUIRE(locals[0]->name() == "local_cmd");

    auto prompts = registry.get_by_type("prompt");
    REQUIRE(prompts.size() == 1);
    REQUIRE(prompts[0]->name() == "prompt_cmd");
}

// ============================================================
// CommandExecutor parse 测试
// ============================================================

TEST_CASE("CommandExecutor parse", "[command]") {
    SECTION("simple command") {
        auto [name, args] = CommandExecutor::parse("/help");
        REQUIRE(name == "help");
        REQUIRE(args.empty());
    }

    SECTION("command with args") {
        auto [name, args] = CommandExecutor::parse("/model deepseek-chat");
        REQUIRE(name == "model");
        REQUIRE(args == "deepseek-chat");
    }

    SECTION("without slash prefix") {
        auto [name, args] = CommandExecutor::parse("help");
        REQUIRE(name == "help");
        REQUIRE(args.empty());
    }

    SECTION("empty input") {
        auto [name, args] = CommandExecutor::parse("");
        REQUIRE(name.empty());
        REQUIRE(args.empty());
    }

    SECTION("command with multiple spaces in args") {
        auto [name, args] = CommandExecutor::parse("/save my session.json");
        REQUIRE(name == "save");
        REQUIRE(args == "my session.json");
    }
}

// ============================================================
// CommandExecutor execute 测试
// ============================================================

TEST_CASE("CommandExecutor execute unknown command", "[command]") {
    auto registry = std::make_shared<CommandRegistry>();
    CommandExecutor executor(registry);

    CommandContext ctx;
    auto result = executor.execute("/nonexistent", ctx);

    REQUIRE(result.command_name == "nonexistent");
    REQUIRE(result.result.is_error);
    REQUIRE_FALSE(result.result.text.empty());
}

TEST_CASE("CommandExecutor execute local command", "[command]") {
    auto registry = std::make_shared<CommandRegistry>();

    auto cmd = make_local_command("echo", "echo command");
    cmd->set_call([](const std::string& args, const CommandContext&) -> CommandResult {
        return CommandResult::ok("echo: " + args);
    });
    registry->register_command(cmd);

    CommandExecutor executor(registry);
    CommandContext ctx;

    SECTION("without args") {
        auto result = executor.execute("/echo", ctx);
        REQUIRE(result.command_name == "echo");
        REQUIRE(result.result.text == "echo: ");
        REQUIRE_FALSE(result.result.is_error);
        REQUIRE_FALSE(result.should_query);
    }

    SECTION("with args") {
        auto result = executor.execute("/echo hello world", ctx);
        REQUIRE(result.command_name == "echo");
        REQUIRE(result.result.text == "echo: hello world");
    }
}

TEST_CASE("CommandExecutor execute prompt command", "[command]") {
    auto registry = std::make_shared<CommandRegistry>();

    auto cmd = make_prompt_command("ask", "ask command");
    cmd->set_prompt_generator([](const std::string& args, const CommandContext&) -> std::vector<PromptBlock> {
        return {{"text", "Please answer: " + args, {}}};
    });
    registry->register_command(cmd);

    CommandExecutor executor(registry);
    CommandContext ctx;

    auto result = executor.execute("/ask what is 2+2", ctx);
    REQUIRE(result.command_name == "ask");
    REQUIRE(result.result.text == "Please answer: what is 2+2");
    REQUIRE(result.should_query);
}

// ============================================================
// 内置系统命令注册测试
// ============================================================

TEST_CASE("register_system_commands registers 6 commands", "[command]") {
    CommandRegistry registry;
    SystemCommandContext ctx;

    register_system_commands(registry, ctx);

    REQUIRE(registry.size() == 6);
    REQUIRE(registry.exists("help"));
    REQUIRE(registry.exists("exit"));
    REQUIRE(registry.exists("quit"));
    REQUIRE(registry.exists("clear"));
    REQUIRE(registry.exists("regen"));
    REQUIRE(registry.exists("model"));
}

TEST_CASE("builtin help command returns command list", "[command]") {
    auto& cfg = ConfigManager::instance();
    cfg.clear_for_test();
    cfg.set("backend.retry_count", 0);

    auto registry = std::make_shared<CommandRegistry>();
    SystemCommandContext sys_ctx;
    register_system_commands(*registry, sys_ctx);

    CommandExecutor executor(registry);
    CommandContext ctx;

    auto result = executor.execute("/help", ctx);
    REQUIRE(result.command_name == "help");
    REQUIRE_FALSE(result.result.is_error);
    REQUIRE(result.result.text.find("Available commands") != std::string::npos);
    REQUIRE(result.result.text.find("help") != std::string::npos);
    REQUIRE(result.result.text.find("exit") != std::string::npos);

    cfg.clear_for_test();
}

TEST_CASE("builtin exit command calls on_exit callback", "[command]") {
    auto registry = std::make_shared<CommandRegistry>();

    bool exit_called = false;
    SystemCommandContext sys_ctx;
    sys_ctx.on_exit = [&exit_called]() { exit_called = true; };
    register_system_commands(*registry, sys_ctx);

    CommandExecutor executor(registry);
    CommandContext ctx;

    executor.execute("/exit", ctx);
    REQUIRE(exit_called);
}

TEST_CASE("builtin quit command calls on_exit callback", "[command]") {
    auto registry = std::make_shared<CommandRegistry>();

    bool exit_called = false;
    SystemCommandContext sys_ctx;
    sys_ctx.on_exit = [&exit_called]() { exit_called = true; };
    register_system_commands(*registry, sys_ctx);

    CommandExecutor executor(registry);
    CommandContext ctx;

    executor.execute("/quit", ctx);
    REQUIRE(exit_called);
}

TEST_CASE("builtin model command calls on_model_select callback", "[command]") {
    auto registry = std::make_shared<CommandRegistry>();

    bool model_called = false;
    SystemCommandContext sys_ctx;
    sys_ctx.on_model_select = [&model_called]() { model_called = true; };
    register_system_commands(*registry, sys_ctx);

    CommandExecutor executor(registry);
    CommandContext ctx;

    executor.execute("/model", ctx);
    REQUIRE(model_called);
}

TEST_CASE("builtin clear command clears session history", "[command]") {
    auto& cfg = ConfigManager::instance();
    cfg.clear_for_test();
    cfg.set("backend.retry_count", 0);

    auto session = make_test_session();
    auto registry = std::make_shared<CommandRegistry>();

    SystemCommandContext sys_ctx;
    sys_ctx.session = session.get();
    register_system_commands(*registry, sys_ctx);

    CommandExecutor executor(registry);
    CommandContext ctx;

    auto result = executor.execute("/clear", ctx);
    REQUIRE(result.command_name == "clear");
    REQUIRE_FALSE(result.result.is_error);
    REQUIRE(result.result.text.find("cleared") != std::string::npos);

    cfg.clear_for_test();
}

TEST_CASE("builtin clear command errors without session", "[command]") {
    auto registry = std::make_shared<CommandRegistry>();

    SystemCommandContext sys_ctx;
    sys_ctx.session = nullptr;
    register_system_commands(*registry, sys_ctx);

    CommandExecutor executor(registry);
    CommandContext ctx;

    auto result = executor.execute("/clear", ctx);
    REQUIRE(result.result.is_error);
    REQUIRE(result.result.text.find("No active session") != std::string::npos);
}
