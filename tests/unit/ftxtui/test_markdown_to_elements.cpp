/**
 * @file test_markdown_to_elements.cpp
 * @brief markdown_to_elements 单元测试（ftxtui 无头逻辑）
 * @details 把 Element 渲染到固定 Screen 后断言渲染文本。覆盖行内样式、标题/
 *          列表/代码块/分隔线/表格降级、空输入，以及侧栏上下文进度条。
 */

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include "render/markdown_to_elements.h"
#include "vm/message_node.h"

using namespace ftxtui;

namespace {

/// @brief 把 Element 渲染到固定尺寸 Screen，返回逐行拼接的文本（用于子串断言）
std::string render_text(const ftxui::Element& e, int cols = 240, int rows = 80) {
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(cols),
                                        ftxui::Dimension::Fixed(rows));
    ftxui::Render(screen, e);
    std::string out;
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < cols; ++x) out += screen.PixelAt(x, y).character;
        out += '\n';
    }
    return out;
}

}  // namespace

// ============================================================================
// 行内样式
// ============================================================================

TEST_CASE("build_inline_line renders bold and inline code", "[markdown][inline]") {
    auto text = render_text(build_inline_line("**bold** `code` ~del~"));
    REQUIRE(text.find("bold") != std::string::npos);
    REQUIRE(text.find("code") != std::string::npos);
    REQUIRE(text.find("del") != std::string::npos);
}

TEST_CASE("build_inline_line strips markdown markers from output", "[markdown][inline]") {
    auto text = render_text(build_inline_line("a **b** c"));
    // 标记符不应泄漏到渲染文本
    REQUIRE(text.find("**") == std::string::npos);
}

// ============================================================================
// 块级语法
// ============================================================================

TEST_CASE("build_markdown renders heading text", "[markdown][block]") {
    auto text = render_text(build_markdown("### Title", 100));
    REQUIRE(text.find("Title") != std::string::npos);
}

TEST_CASE("build_markdown renders unordered list", "[markdown][block]") {
    auto text = render_text(build_markdown("- item one\n- item two", 100));
    REQUIRE(text.find("item one") != std::string::npos);
    REQUIRE(text.find("item two") != std::string::npos);
}

TEST_CASE("build_markdown renders ordered list", "[markdown][block]") {
    auto text = render_text(build_markdown("1. first\n2. second", 100));
    REQUIRE(text.find("first") != std::string::npos);
    REQUIRE(text.find("second") != std::string::npos);
}

TEST_CASE("build_markdown renders code block with language tag", "[markdown][block]") {
    auto text = render_text(
        build_markdown("```cpp\nint main() {}\n```", 100));
    REQUIRE(text.find("cpp") != std::string::npos);
    REQUIRE(text.find("int main()") != std::string::npos);
}

TEST_CASE("build_markdown does not throw on horizontal rule", "[markdown][block]") {
    REQUIRE_NOTHROW(render_text(build_markdown("---", 100)));
}

TEST_CASE("build_markdown degrades table row to inline text", "[markdown][block]") {
    auto text = render_text(build_markdown("| a | b |", 100));
    REQUIRE(text.find("a") != std::string::npos);
    REQUIRE(text.find("b") != std::string::npos);
}

TEST_CASE("build_markdown empty input renders empty", "[markdown][block]") {
    auto text = render_text(build_markdown("", 100));
    // 空输入不应产生可见的打印内容（仅空白填充）
    auto has_printable = [](const std::string& s) {
        return std::any_of(s.begin(), s.end(),
                           [](char c) { return c != ' ' && c != '\n' && c != '\t'; });
    };
    REQUIRE_FALSE(has_printable(text));
}

// ============================================================================
// 侧栏上下文进度条
// ============================================================================

TEST_CASE("build_context_gauge renders n/a when no limit", "[markdown][gauge]") {
    auto text = render_text(build_context_gauge(0, 0));
    REQUIRE(text.find("n/a") != std::string::npos);
}

TEST_CASE("build_context_gauge renders gauge when limit present", "[markdown][gauge]") {
    auto text = render_text(build_context_gauge(5, 10));
    REQUIRE(text.find("n/a") == std::string::npos);
}