/**
 * @file test_skill_prompt.cpp
 * @brief skills 提示词小节生成单元测试
 */

#include <catch2/catch_test_macros.hpp>

#include "agent/skill/inclaude/skill_prompt.h"

using namespace agent::skill;
using namespace agent::command;

namespace {

std::shared_ptr<PromptCommand> make_cmd(const std::string& name,
                                        const std::string& description,
                                        LoadSource source,
                                        bool disable_model = false,
                                        std::optional<std::string> when_to_use = std::nullopt) {
    auto cmd = std::make_shared<PromptCommand>(name, description);
    cmd->set_loaded_from(source);
    cmd->set_disable_model_invocation(disable_model);
    if (when_to_use) cmd->set_when_to_use(*when_to_use);
    return cmd;
}

} // anonymous namespace

TEST_CASE("empty registry produces empty section", "[skill][prompt]") {
    CommandRegistry registry;
    REQUIRE(build_skills_prompt_section(registry).empty());
}

TEST_CASE("skills are listed with name and description", "[skill][prompt]") {
    CommandRegistry registry;
    registry.register_command(make_cmd("build", "Compiles the project", LoadSource::Skills));

    const auto section = build_skills_prompt_section(registry);

    REQUIRE(section.find("# Available skills") != std::string::npos);
    REQUIRE(section.find("- build: Compiles the project") != std::string::npos);
    REQUIRE(section.find("Skill tool") != std::string::npos);
}

TEST_CASE("when_to_use is appended when present", "[skill][prompt]") {
    CommandRegistry registry;
    registry.register_command(make_cmd("review", "Reviews code", LoadSource::Skills,
                                       false, std::string("When doing code review")));

    const auto section = build_skills_prompt_section(registry);

    REQUIRE(section.find("(when to use: When doing code review)") != std::string::npos);
}

TEST_CASE("description-empty skill uses when_to_use", "[skill][prompt]") {
    CommandRegistry registry;
    registry.register_command(make_cmd("debug", "", LoadSource::Skills,
                                       false, std::string("When debugging")));

    const auto section = build_skills_prompt_section(registry);

    REQUIRE(section.find("- debug: When debugging") != std::string::npos);
}

TEST_CASE("model-invocation-disabled skills are filtered", "[skill][prompt]") {
    CommandRegistry registry;
    registry.register_command(make_cmd("secret", "Hidden", LoadSource::Skills, true));

    REQUIRE(build_skills_prompt_section(registry).empty());
}

TEST_CASE("non-skills commands are filtered", "[skill][prompt]") {
    CommandRegistry registry;
    registry.register_command(make_cmd("help", "Shows help", LoadSource::Builtin));

    REQUIRE(build_skills_prompt_section(registry).empty());
}
