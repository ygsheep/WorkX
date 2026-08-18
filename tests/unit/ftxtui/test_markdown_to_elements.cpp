/**
 * @file test_markdown_to_elements.cpp
 * @brief markdown_to_elements 单元测试（ftxtui 无头逻辑）
 * @details 把 Element 渲染到固定 Screen 后断言渲染文本。覆盖行内样式、标题/
 *          列表/代码块/分隔线/表格降级、空输入，以及侧栏上下文进度条。
 */

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <format>
#include <ranges>
#include <string>
#include <string_view>

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

TEST_CASE("build_markdown renders table block with box-drawing borders", "[markdown][block]") {
    auto text = render_text(build_markdown(
        "| 场景 | 复杂度 |\n"
        "| --- | --- |\n"
        "| 平均 | O(n log n) |\n"
        "| 最坏 | O(n^2) |", 100));
    // 边框字符（┌ ┬ ┐ ├ ┼ ┤ └ ┴ ┘ │）
    REQUIRE(text.find("\u250c") != std::string::npos);
    REQUIRE(text.find("\u251c") != std::string::npos);
    REQUIRE(text.find("\u2514") != std::string::npos);
    REQUIRE(text.find("\u2502") != std::string::npos);
    // 边框横线 ─（U+2500，多字节字符，检查不出现截断乱码）
    REQUIRE(text.find("\u2500") != std::string::npos);
    // 表头与数据可见
    REQUIRE(text.find("场景") != std::string::npos);
    REQUIRE(text.find("复杂度") != std::string::npos);
    REQUIRE(text.find("O(n log n)") != std::string::npos);
    // 分隔行（| --- | --- |）不应以原文泄漏
    REQUIRE(text.find("---") == std::string::npos);
}

TEST_CASE("build_markdown renders table alignment separators", "[markdown][block]") {
    auto text = render_text(build_markdown(
        "| a | b | c |\n"
        "| :--- | :---: | ---: |\n"
        "| 1 | 2 | 3 |", 100));
    REQUIRE(text.find("a") != std::string::npos);
    REQUIRE(text.find("1") != std::string::npos);
    REQUIRE(text.find("---") == std::string::npos);
}

TEST_CASE("build_markdown renders bold inside table cells", "[markdown][block]") {
    auto text = render_text(build_markdown(
        "| 状态 | 说明 |\n"
        "| --- | --- |\n"
        "| **成功** | 一切正常 |\n"
        "| 失败 | **重试** 即可 |", 100));
    // 单元格行内解析：** 被消费，文字保留（对齐 src/tui render_inline 行为）
    REQUIRE(text.find("**") == std::string::npos);
    REQUIRE(text.find("成功") != std::string::npos);
    REQUIRE(text.find("重试") != std::string::npos);
    REQUIRE(text.find("一切正常") != std::string::npos);
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

// ============================================================================
// 工具结果特化渲染（Read 预览 / Write/Edit diff）
// ============================================================================

namespace {

/// @brief 构造一个展开的 Read/Write 工具卡消息
MessageNode make_tool_message(const char* tool, const char* result) {
    ToolCallNode t;
    t.tool_name = tool;
    t.arguments = R"({"file_path": "src/main.cpp"})";
    t.result = result;
    t.done = true;
    t.expanded = true;
    MessageNode msg;
    msg.tool_calls.push_back(t);
    return msg;
}

}  // namespace

TEST_CASE("build_message renders Read preview with line numbers and metadata",
          "[markdown][toolcard]") {
    auto msg = make_tool_message("Read",
        "  1\u2192int main() {\n"
        "  2\u2192    return 0;\n"
        "  3\u2192}\n"
        "\n"
        "(read lines 1-3, total 3, truncated)");
    auto text = render_text(build_message(msg, 100));
    // 状态行 + 行号前缀（│1）+ 代码内容 + 元数据行
    REQUIRE(text.find("has been read successfully") != std::string::npos);
    REQUIRE(text.find("\u25021") != std::string::npos);
    REQUIRE(text.find("return 0") != std::string::npos);
    REQUIRE(text.find("(read lines 1-3, total 3, truncated)") != std::string::npos);
}

TEST_CASE("build_message renders Write diff with status text and bg prefix",
          "[markdown][toolcard]") {
    auto msg = make_tool_message("Write",
        "File updated.\n"
        "\n"
        "--- a/src/main.cpp\n"
        "+++ b/src/main.cpp\n"
        "@@ -1,3 +1,3 @@\n"
        "-int main() {\n"
        "+int main() {\n"
        "     return 0;\n");
    auto text = render_text(build_message(msg, 100));
    // 状态文本可见；diff 头行（---/+++/@@）不泄漏；+ 行内容与序号可见
    REQUIRE(text.find("File updated.") != std::string::npos);
    REQUIRE(text.find("--- a/src/main.cpp") == std::string::npos);
    REQUIRE(text.find("+++ b/src/main.cpp") == std::string::npos);
    REQUIRE(text.find("@@ -1,3 +1,3 @@") == std::string::npos);
    REQUIRE(text.find("int main()") != std::string::npos);
    REQUIRE(text.find("\u25021") != std::string::npos);
}

TEST_CASE("build_message truncates long Read results at 60 lines",
          "[markdown][toolcard]") {
    std::string result;
    for (int i = 1; i <= 62; ++i)
        result += std::format("{:>2}\u2192line{}\n", i, i);
    auto msg = make_tool_message("Read", result.c_str());
    auto text = render_text(build_message(msg, 100), 240, 100);
    REQUIRE(text.find("(... truncated") != std::string::npos);
    REQUIRE(text.find("line60") != std::string::npos);
    REQUIRE(text.find("line61") == std::string::npos);
    REQUIRE(text.find("line62") == std::string::npos);
}

TEST_CASE("estimate_message_height syncs with Read tool result layout",
          "[markdown][toolcard]") {
    auto msg = make_tool_message("Read",
        "  1\u2192int main() {\n"
        "  2\u2192    return 0;\n"
        "  3\u2192}\n"
        "\n"
        "(read lines 1-3, total 3, truncated)");
    // 3(边框+头) + 1(fpath 行) + 1(状态行) + 3(代码) + 2(vPad) + 2(元数据)
    REQUIRE(estimate_message_height(msg) == 12);
}

// ============================================================================
// 代码块上下留白（README「代码上下一行距离」）
// ============================================================================

TEST_CASE("build_markdown code block keeps vertical padding", "[markdown][block]") {
    // vPad：上下各 1 空行 → 1 代码行 + 1 语言行 + 2 留白 = 4
    REQUIRE(estimate_markdown_height("```cpp\nint x;\n```") == 4);
    auto text = render_text(build_markdown("```cpp\nint x;\n```", 100));
    REQUIRE(text.find("int x;") != std::string::npos);
    // 渲染出的有效行数 = 4（语言行 + 代码行 + 上下留白）
    int visible = 0;
    for (const auto& row : text | std::views::split('\n')) {
        std::string_view sv(row.begin(), row.end());
        if (!sv.empty() && std::any_of(sv.begin(), sv.end(),
            [](char c) { return c != ' '; })) ++visible;
    }
    REQUIRE(visible == 4);
}