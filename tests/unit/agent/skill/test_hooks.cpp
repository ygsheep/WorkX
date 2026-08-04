/**
 * @file test_hooks.cpp
 * @brief Skill PreActivate 钩子执行器单元测试
 */

#include <catch2/catch_test_macros.hpp>

#include "agent/skill/inclaude/hooks.h"

using namespace agent::skill;

TEST_CASE("run_preactivate_hooks executes commands and captures output", "[skill][hooks]") {
    const std::vector<std::string> hooks{
#ifdef _WIN32
        "echo hook-output-123",
#else
        "echo hook-output-123",
#endif
    };

    const auto lines = run_preactivate_hooks(hooks, ".");

    REQUIRE(lines.size() == 1);
    REQUIRE(lines[0].find("[ok]") != std::string::npos);
    REQUIRE(lines[0].find("hook-output-123") != std::string::npos);
}

TEST_CASE("run_preactivate_hooks reports failures without stopping", "[skill][hooks]") {
    const std::vector<std::string> hooks{
#ifdef _WIN32
        "no-such-command-xyz",
#else
        "no-such-command-xyz",
#endif
        "echo second-hook-ok",
    };

    const auto lines = run_preactivate_hooks(hooks, ".");

    REQUIRE(lines.size() == 2);
    REQUIRE(lines[0].find("[fail]") != std::string::npos);
    REQUIRE(lines[1].find("[ok]") != std::string::npos);
    REQUIRE(lines[1].find("second-hook-ok") != std::string::npos);
}

TEST_CASE("run_preactivate_hooks skips empty commands", "[skill][hooks]") {
    REQUIRE(run_preactivate_hooks({}, ".").empty());
    REQUIRE(run_preactivate_hooks({"", ""}, ".").empty());
}

TEST_CASE("format_hook_output indents lines", "[skill][hooks]") {
    const auto text = format_hook_output({"[ok] echo a", "[fail] boom"});
    REQUIRE(text.find("  [ok] echo a") != std::string::npos);
    REQUIRE(text.find("  [fail] boom") != std::string::npos);
    REQUIRE(format_hook_output({}).empty());
}
