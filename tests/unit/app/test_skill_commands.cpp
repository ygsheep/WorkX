/**
 * @file test_skill_commands.cpp
 * @brief register_skill_commands 端到端测试（加载仓库真实 .claude/skills）
 */

#include <catch2/catch_test_macros.hpp>

#include "app/command/skill_commands.h"
#include "agent/command/inclaude/command.h"
#include "agent/command/inclaude/executor.h"

using namespace agent::command;

TEST_CASE("register_skill_commands loads repo skills", "[skill][commands]") {
    CommandRegistry registry;
    register_skill_commands(registry, SOURCE_DIR);

    const auto cmd = registry.find_by_name("workx-build");
    REQUIRE(cmd != nullptr);
    REQUIRE(cmd->type() == "prompt");
    REQUIRE(cmd->loaded_from() == LoadSource::Skills);
    REQUIRE_FALSE(cmd->description().empty());

    // 别名同样可解析
    const auto alias = registry.find_by_name("wb");
    REQUIRE(alias != nullptr);

    // 提示词内容含参考文件指引（baseDir 前缀）
    const auto* prompt_cmd = dynamic_cast<const PromptCommand*>(cmd.get());
    REQUIRE(prompt_cmd != nullptr);
    const auto blocks = prompt_cmd->generate_prompt("", CommandContext{});
    REQUIRE_FALSE(blocks.empty());
    REQUIRE(blocks[0].text.find("Base directory for this skill:") != std::string::npos);
    REQUIRE(blocks[0].text.find("cmake --preset default") != std::string::npos);
}

TEST_CASE("skill command execution requests model query", "[skill][commands]") {
    auto registry = std::make_shared<CommandRegistry>();
    register_skill_commands(*registry, SOURCE_DIR);

    CommandExecutor executor(registry);
    const auto result = executor.execute("/workx-build", CommandContext{});

    // PromptCommand 展开后必须请求模型查询（而非本地回显），
    // 否则 skill 内容不会发送给模型（回归：main.cpp 本地回显短路）
    REQUIRE(result.should_query);
    REQUIRE_FALSE(result.result.text.empty());
    REQUIRE(result.result.text.find("cmake --preset default") != std::string::npos);

    // 本地命令仍保持 should_query=false
    CommandExecutor executor2(registry);
    const auto clear_result = executor2.execute("/clear", CommandContext{});
    REQUIRE_FALSE(clear_result.should_query);
}
