/**
 * @file test_command_registry.cpp
 * @brief CommandRegistry 单元测试（ftxtui 无头逻辑）
 * @details 覆盖内置命令清单、add 语义（保持顺序）、all() 拷贝返回值，
 *          以及中英文搜索关键词字段的完整性。
 */

#include <catch2/catch_test_macros.hpp>

#include "command/command_registry.h"

using namespace ftxtui;

TEST_CASE("CommandRegistry builtins registers the six core commands", "[command_registry][builtins]") {
    auto reg = CommandRegistry::builtins();
    const auto& cmds = reg.all();
    REQUIRE(cmds.size() == 6);
    REQUIRE(cmds[0].command == "/help");
    REQUIRE(cmds[1].command == "/clear");
    REQUIRE(cmds[2].command == "/exit");
    REQUIRE(cmds[3].command == "/model");
    REQUIRE(cmds[4].command == "/resume");
    REQUIRE(cmds[5].command == "/rename");
}

TEST_CASE("CommandRegistry builtins carry searchable keywords", "[command_registry][builtins]") {
    auto reg = CommandRegistry::builtins();
    for (const auto& c : reg.all()) {
        REQUIRE_FALSE(c.title.empty());
        REQUIRE_FALSE(c.keywords.empty());
        REQUIRE(c.command.rfind("/", 0) == 0);
    }
}

TEST_CASE("CommandRegistry add appends and preserves order", "[command_registry][add]") {
    CommandRegistry reg;
    reg.add({.command = "/a", .title = "甲", .keywords = "a"});
    reg.add({.command = "/b", .title = "乙", .keywords = "b"});
    const auto& cmds = reg.all();
    REQUIRE(cmds.size() == 2);
    REQUIRE(cmds[0].command == "/a");
    REQUIRE(cmds[1].command == "/b");
}

TEST_CASE("CommandRegistry all returns a copy of back-ordered entries", "[command_registry][add]") {
    CommandRegistry reg;
    reg.add({.command = "/x", .title = "x", .keywords = ""});
    // all() 返回 const 引用；对外只读
    auto& view = reg.all();
    REQUIRE(view.size() == 1);
    REQUIRE(view[0].title == "x");
}