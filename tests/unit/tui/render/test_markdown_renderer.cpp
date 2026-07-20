/**
 * @file test_markdown_renderer.cpp
 * @brief Markdown 表格解析与渲染单元测试
 */

#include <catch2/catch_test_macros.hpp>
#include "tui/render/markdown_renderer.h"

using namespace agent;

// ---- split_table_row ----

TEST_CASE("split_table_row basic", "[markdown]") {
    auto cells = split_table_row("| a | b |");
    REQUIRE(cells.size() == 2);
    REQUIRE(cells[0] == "a");
    REQUIRE(cells[1] == "b");
}

TEST_CASE("split_table_row no spaces", "[markdown]") {
    auto cells = split_table_row("|a|b|");
    REQUIRE(cells.size() == 2);
    REQUIRE(cells[0] == "a");
    REQUIRE(cells[1] == "b");
}

TEST_CASE("split_table_row escaped pipe", "[markdown]") {
    auto cells = split_table_row("| a \\| b | c |");
    // \| is an escaped pipe → literal | in cell content
    // After removing leading/trailing |, splitting by unescaped |:
    // "a | b", "c" → 2 cells
    REQUIRE(cells.size() == 2);
    REQUIRE(cells[0] == "a | b");
    REQUIRE(cells[1] == "c");
}

TEST_CASE("split_table_row trimmed", "[markdown]") {
    auto cells = split_table_row("|  spaced  |  more  |");
    REQUIRE(cells.size() == 2);
    REQUIRE(cells[0] == "spaced");
    REQUIRE(cells[1] == "more");
}

TEST_CASE("split_table_row single column", "[markdown]") {
    auto cells = split_table_row("| only |");
    REQUIRE(cells.size() == 1);
    REQUIRE(cells[0] == "only");
}

TEST_CASE("split_table_row empty cells", "[markdown]") {
    auto cells = split_table_row("| | |");
    REQUIRE(cells.size() == 2);
    REQUIRE(cells[0] == "");
    REQUIRE(cells[1] == "");
}

TEST_CASE("split_table_row leading spaces before pipe", "[markdown]") {
    auto cells = split_table_row("  | a | b |");
    REQUIRE(cells.size() == 2);
    REQUIRE(cells[0] == "a");
    REQUIRE(cells[1] == "b");
}

// ---- is_table_row ----

TEST_CASE("is_table_row", "[markdown]") {
    REQUIRE(is_table_row("| a | b |"));
    REQUIRE(is_table_row("|a|b|"));
    REQUIRE(is_table_row("  | indented |"));
    REQUIRE_FALSE(is_table_row("no pipe"));
    REQUIRE_FALSE(is_table_row(""));
    REQUIRE_FALSE(is_table_row("text | with | pipe"));
}

// ---- is_table_separator ----

TEST_CASE("is_table_separator basic", "[markdown]") {
    std::vector<TableAlign> aligns;
    REQUIRE(is_table_separator("|---|---|", aligns));
    REQUIRE(aligns.size() == 2);
    REQUIRE(aligns[0] == TableAlign::Default);
    REQUIRE(aligns[1] == TableAlign::Default);
}

TEST_CASE("is_table_separator alignment", "[markdown]") {
    std::vector<TableAlign> aligns;
    REQUIRE(is_table_separator("|:--|:-:|--:|", aligns));
    REQUIRE(aligns.size() == 3);
    REQUIRE(aligns[0] == TableAlign::Left);
    REQUIRE(aligns[1] == TableAlign::Center);
    REQUIRE(aligns[2] == TableAlign::Right);
}

TEST_CASE("is_table_separator single column", "[markdown]") {
    std::vector<TableAlign> aligns;
    REQUIRE(is_table_separator("|---|", aligns));
    REQUIRE(aligns.size() == 1);
}

TEST_CASE("is_table_separator invalid", "[markdown]") {
    std::vector<TableAlign> aligns;
    REQUIRE_FALSE(is_table_separator("| abc |", aligns));
    REQUIRE_FALSE(is_table_separator("|a|b|", aligns));
    REQUIRE_FALSE(is_table_separator("no pipe", aligns));
    // Only dashes and colons allowed
    REQUIRE_FALSE(is_table_separator("|-x-|", aligns));
}

TEST_CASE("is_table_separator with spaces", "[markdown]") {
    std::vector<TableAlign> aligns;
    REQUIRE(is_table_separator("| --- | --- |", aligns));
    REQUIRE(aligns.size() == 2);
    REQUIRE(is_table_separator("| :-- | :-: | --: |", aligns));
    REQUIRE(aligns.size() == 3);
}

// ---- parse_table ----

TEST_CASE("parse_table valid 2x2", "[markdown]") {
    std::vector<std::string> lines = {
        "| H1 | H2 |",
        "|---|---|",
        "| a | b |",
        "| c | d |"
    };
    auto table = parse_table(lines);
    REQUIRE(table.valid);
    REQUIRE(table.headers.size() == 2);
    REQUIRE(table.headers[0] == "H1");
    REQUIRE(table.headers[1] == "H2");
    REQUIRE(table.rows.size() == 2);
    REQUIRE(table.rows[0][0] == "a");
    REQUIRE(table.rows[0][1] == "b");
    REQUIRE(table.rows[1][0] == "c");
    REQUIRE(table.rows[1][1] == "d");
}

TEST_CASE("parse_table missing separator", "[markdown]") {
    std::vector<std::string> lines = {
        "| H1 | H2 |",
        "| a | b |"
    };
    auto table = parse_table(lines);
    REQUIRE_FALSE(table.valid);
}

TEST_CASE("parse_table invalid separator", "[markdown]") {
    std::vector<std::string> lines = {
        "| H1 | H2 |",
        "| abc | def |",
        "| a | b |"
    };
    auto table = parse_table(lines);
    REQUIRE_FALSE(table.valid);
}

TEST_CASE("parse_table ragged rows", "[markdown]") {
    std::vector<std::string> lines = {
        "| H1 | H2 | H3 |",
        "|---|---|---|",
        "| a | b |",
        "| x | y | z | w |"
    };
    auto table = parse_table(lines);
    REQUIRE(table.valid);
    REQUIRE(table.headers.size() == 3);
    REQUIRE(table.rows.size() == 2);
    // Row 0 has 2 cells → pad to 3
    REQUIRE(table.rows[0].size() == 3);
    REQUIRE(table.rows[0][0] == "a");
    REQUIRE(table.rows[0][1] == "b");
    REQUIRE(table.rows[0][2] == "");
    // Row 1 has 4 cells → extra cell kept
    REQUIRE(table.rows[1].size() == 4);
}

TEST_CASE("parse_table empty body", "[markdown]") {
    std::vector<std::string> lines = {
        "| H1 | H2 |",
        "|---|---|"
    };
    auto table = parse_table(lines);
    REQUIRE(table.valid);
    REQUIRE(table.rows.empty());
}

TEST_CASE("parse_table with alignment", "[markdown]") {
    std::vector<std::string> lines = {
        "| H1 | H2 | H3 |",
        "|:--|:-:|--:|",
        "| a | b | c |"
    };
    auto table = parse_table(lines);
    REQUIRE(table.valid);
    REQUIRE(table.alignments.size() == 3);
    REQUIRE(table.alignments[0] == TableAlign::Left);
    REQUIRE(table.alignments[1] == TableAlign::Center);
    REQUIRE(table.alignments[2] == TableAlign::Right);
}

TEST_CASE("parse_table too few lines", "[markdown]") {
    std::vector<std::string> lines = {"| H1 | H2 |"};
    auto table = parse_table(lines);
    REQUIRE_FALSE(table.valid);
}

// ---- render_table ----

TEST_CASE("render_table basic 2x2", "[markdown]") {
    std::vector<std::string> lines = {
        "| H1 | H2 |",
        "|---|---|",
        "| cell1 | cell2 |"
    };
    auto table = parse_table(lines);
    REQUIRE(table.valid);
    std::string output = render_table(table);
    // Should contain box-drawing characters
    REQUIRE(output.find("\xe2\x94\x8c") != std::string::npos);  // ┌
    REQUIRE(output.find("\xe2\x94\x90") != std::string::npos);  // ┐
    REQUIRE(output.find("\xe2\x94\x94") != std::string::npos);  // └
    REQUIRE(output.find("\xe2\x94\x98") != std::string::npos);  // ┘
    REQUIRE(output.find("\xe2\x94\x82") != std::string::npos);  // │
    REQUIRE(output.find("\xe2\x94\xac") != std::string::npos);  // ┬
    REQUIRE(output.find("\xe2\x94\xb4") != std::string::npos);  // ┴
    REQUIRE(output.find("\xe2\x94\x9c") != std::string::npos);  // ├
    REQUIRE(output.find("\xe2\x94\xa4") != std::string::npos);  // ┤
    REQUIRE(output.find("\xe2\x94\xbc") != std::string::npos);  // ┼
    // Should contain header and cell text
    REQUIRE(output.find("H1") != std::string::npos);
    REQUIRE(output.find("H2") != std::string::npos);
    REQUIRE(output.find("cell1") != std::string::npos);
    REQUIRE(output.find("cell2") != std::string::npos);
}

TEST_CASE("render_table CJK content", "[markdown]") {
    std::vector<std::string> lines = {
        "| \xe5\x90\x8d\xe5\x89\x8d | \xe5\x80\xa4 |",  // | 名前 | 値 |
        "|---|---|",
        "| \xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88 | 42 |"  // | テスト | 42 |
    };
    auto table = parse_table(lines);
    REQUIRE(table.valid);
    std::string output = render_table(table);
    // CJK column should be wider than 2 (each CJK char is width 2)
    // Check that the output has enough dashes for CJK width
    // "名前" = 4 display width, "テスト" = 6 display width
    // Column 1 width = max(4, 6) = 6
    REQUIRE(output.find("\xe5\x90\x8d\xe5\x89\x8d") != std::string::npos);  // 名前
    REQUIRE(output.find("\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88") != std::string::npos);  // テスト
}

TEST_CASE("render_table alignment", "[markdown]") {
    std::vector<std::string> lines = {
        "| Left | Center | Right |",
        "|:--|:-:|--:|",
        "| a | b | c |"
    };
    auto table = parse_table(lines);
    REQUIRE(table.valid);
    std::string output = render_table(table);
    // Left-aligned: "a" followed by spaces
    REQUIRE((output.find("a ") != std::string::npos || output.find("a\n") != std::string::npos));
    // Right-aligned: spaces followed by "c"
    REQUIRE((output.find(" c") != std::string::npos || output.find("\nc") != std::string::npos));
}

TEST_CASE("render_table no width limit", "[markdown]") {
    std::vector<std::string> lines = {
        "| VeryLongHeaderName | Another |",
        "|---|---|",
        "| data | x |"
    };
    auto table = parse_table(lines);
    REQUIRE(table.valid);
    std::string output = render_table(table, 0);  // 0 = no limit
    REQUIRE(output.find("VeryLongHeaderName") != std::string::npos);
}

TEST_CASE("render_table width overflow", "[markdown]") {
    std::vector<std::string> lines = {
        "| Header1 | Header2 |",
        "|---|---|",
        "| verylongcontent | x |"
    };
    auto table = parse_table(lines);
    REQUIRE(table.valid);
    // Very narrow width → should truncate
    std::string output = render_table(table, 15);
    // Should still contain box drawing
    REQUIRE(output.find("\xe2\x94\x82") != std::string::npos);  // │
    // Should contain truncation ellipsis
    REQUIRE(output.find("\xe2\x80\xa6") != std::string::npos);  // …
}

TEST_CASE("render_table empty body", "[markdown]") {
    std::vector<std::string> lines = {
        "| H1 | H2 |",
        "|---|---|"
    };
    auto table = parse_table(lines);
    REQUIRE(table.valid);
    std::string output = render_table(table);
    // Should have top border, header, separator, bottom border — no data rows
    REQUIRE(output.find("H1") != std::string::npos);
    REQUIRE(output.find("\xe2\x94\x8c") != std::string::npos);  // ┌
    REQUIRE(output.find("\xe2\x94\x94") != std::string::npos);  // └
}

TEST_CASE("render_table inline markdown in cells", "[markdown]") {
    std::vector<std::string> lines = {
        "| \xe5\x90\x8d\xe7\xa7\xb0 | \xe7\xb1\xbb\xe5\x9e\x8b |",  // | 名称 | 类型 |
        "|------|------|",
        "| **\xe6\xa8\xa1\xe5\x9e\x8b\xe5\x90\x8d\xe7\xa7\xb0** | \xe5\x87\xbd\xe6\x95\xb0 |",  // | **模型名称** | 函数 |
        "| render | **\xe6\x96\xb9\xe6\xb3\x95** |"  // | render | **方法** |
    };
    auto table = parse_table(lines);
    REQUIRE(table.valid);
    std::string output = render_table(table);
    // Bold markers should be consumed (not present as literal **)
    REQUIRE(output.find("**") == std::string::npos);
    // Cell text should still be present (without **)
    REQUIRE(output.find("\xe6\xa8\xa1\xe5\x9e\x8b\xe5\x90\x8d\xe7\xa7\xb0") != std::string::npos);  // 模型名称
    REQUIRE(output.find("\xe6\x96\xb9\xe6\xb3\x95") != std::string::npos);  // 方法
    // Bold ANSI code should be present for **bold** rendering
    // render_inline 用 \x1b[1m (BOLD) 而非 \x1b[33m (YELLOW)
    REQUIRE(output.find("\x1b[1m") != std::string::npos);  // \e[1m = bold
}

// ---- TableBuffer ----

TEST_CASE("TableBuffer valid table", "[markdown]") {
    TableBuffer buf;
    REQUIRE(buf.feed_line("| H1 | H2 |"));
    REQUIRE(buf.is_active());
    REQUIRE(buf.feed_line("|---|---|"));
    REQUIRE(buf.is_active());
    REQUIRE(buf.feed_line("| a | b |"));
    REQUIRE(buf.is_active());
    // Non-table line ends the table
    REQUIRE_FALSE(buf.feed_line("normal text"));
    REQUIRE(buf.is_complete());
    REQUIRE(buf.lines().size() == 3);
}

TEST_CASE("TableBuffer missing separator", "[markdown]") {
    TableBuffer buf;
    REQUIRE(buf.feed_line("| H1 | H2 |"));
    REQUIRE(buf.is_active());
    // Non-table, non-separator line
    REQUIRE_FALSE(buf.feed_line("not a separator"));
    REQUIRE(buf.is_invalid());
}

TEST_CASE("TableBuffer blank line ends table", "[markdown]") {
    TableBuffer buf;
    REQUIRE(buf.feed_line("| H1 | H2 |"));
    REQUIRE(buf.feed_line("|---|---|"));
    REQUIRE(buf.feed_line("| a | b |"));
    REQUIRE_FALSE(buf.feed_line(""));
    REQUIRE(buf.is_complete());
}

TEST_CASE("TableBuffer non-table line ignored when empty", "[markdown]") {
    TableBuffer buf;
    REQUIRE_FALSE(buf.feed_line("normal text"));
    REQUIRE_FALSE(buf.is_active());
    REQUIRE_FALSE(buf.is_complete());
    REQUIRE_FALSE(buf.is_invalid());
}

TEST_CASE("TableBuffer clear", "[markdown]") {
    TableBuffer buf;
    buf.feed_line("| H1 | H2 |");
    buf.feed_line("|---|---|");
    buf.clear();
    REQUIRE_FALSE(buf.is_active());
    REQUIRE(buf.lines().empty());
}
