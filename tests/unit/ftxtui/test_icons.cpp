/**
 * @file test_icons.cpp
 * @brief 图标层单测（A5）：Nerd Font 默认开启；关闭后降级 ASCII/空。
 * @note theme::set_nerd_font 为全局状态，各用例自行设置前后置状态。
 */

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "theme/icons.h"

using namespace ftxtui;

TEST_CASE("nerd font enabled by default", "[icons]") {
    theme::set_nerd_font(true);
    REQUIRE(theme::nerd_font());
}

TEST_CASE("nerd font glyphs when enabled", "[icons]") {
    theme::set_nerd_font(true);
    REQUIRE(theme::icon_think() == "\uF0EB");
    REQUIRE(theme::icon_chevron_down() == "\uF078");
    REQUIRE(theme::icon_chevron_right() == "\uF054");
    REQUIRE(theme::icon_tool() == "\uF0AD");
    REQUIRE(theme::icon_plan() == "\uF044");
    REQUIRE(theme::icon_full_access() == "\uEB53");
    REQUIRE(theme::icon_manual() == "\uF256");
}

TEST_CASE("ascii fallback when disabled", "[icons]") {
    theme::set_nerd_font(false);
    REQUIRE_FALSE(theme::nerd_font());
    REQUIRE(theme::icon_think() == "");
    REQUIRE(theme::icon_chevron_down() == "v");
    REQUIRE(theme::icon_chevron_right() == ">");
    REQUIRE(theme::icon_tool() == "");
    REQUIRE(theme::icon_plan() == "P");
    REQUIRE(theme::icon_full_access() == "A");
    REQUIRE(theme::icon_manual() == "?");
    theme::set_nerd_font(true);
}

TEST_CASE("icon toggles back to glyphs", "[icons]") {
    theme::set_nerd_font(false);
    REQUIRE(theme::icon_chevron_down() == "v");
    theme::set_nerd_font(true);
    REQUIRE(theme::icon_chevron_down() == "\uF078");
}