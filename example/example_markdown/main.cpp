/**
 * @file main.cpp
 * @brief Markdown -> TUI 字符画渲染示例
 * @details 读取 .md 文件，逐行解析 Markdown 语法，
 *          用 ANSI 颜色 + Unicode box-drawing 字符渲染到终端。
 *          复用 workx::parse_table / display_width 纯函数。
 */

#include <iostream>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

#include "tui/render/markdown_renderer.h"
#include "tui/utils/utf8_utils.h"

using namespace tui;

// ============================================================================
// ANSI 颜色常量
// ============================================================================

namespace ansi {
    constexpr auto RESET     = "\x1b[0m";
    constexpr auto BOLD      = "\x1b[1m";
    constexpr auto DIM       = "\x1b[2m";
    constexpr auto ITALIC    = "\x1b[3m";
    constexpr auto UNDERLINE = "\x1b[4m";
    constexpr auto STRIKE    = "\x1b[9m";
    constexpr auto RED       = "\x1b[31m";
    constexpr auto GREEN     = "\x1b[32m";
    constexpr auto YELLOW    = "\x1b[33m";
    constexpr auto BLUE      = "\x1b[34m";
    constexpr auto MAGENTA   = "\x1b[35m";
    constexpr auto CYAN      = "\x1b[36m";
    constexpr auto GRAY      = "\x1b[90m";   // 淡白色（亮灰）
    constexpr auto WHITE     = "\x1b[97m";   // 亮白色
}

// ============================================================================
// Box-drawing 字符 (UTF-8 字节序列)
// ============================================================================

namespace box {
    constexpr auto TL = "\xe2\x94\x8c";  // ┌
    constexpr auto TR = "\xe2\x94\x90";  // ┐
    constexpr auto BL = "\xe2\x94\x94";  // └
    constexpr auto BR = "\xe2\x94\x98";  // ┘
    constexpr auto H  = "\xe2\x94\x80";  // ─
    constexpr auto V  = "\xe2\x94\x82";  // │
    constexpr auto LT = "\xe2\x94\xac";  // ┬
    constexpr auto RT = "\xe2\x94\xb4";  // ┴
    constexpr auto LV = "\xe2\x94\x9c";  // ├
    constexpr auto RV = "\xe2\x94\xa4";  // ┤
    constexpr auto X  = "\xe2\x94\xbc";  // ┼
    constexpr auto DH = "\xe2\x95\x90";  // ═ (双横线)
    constexpr auto BULLET = "\xe2\x80\xa2";  // •
}

// ============================================================================
// 表格彩色渲染 (核心亮点, 仅示例特有)
// ============================================================================

static void render_table_colored(const std::vector<std::string>& lines) {
    auto table = parse_table(lines);
    if (!table.valid) {
        for (const auto& l : lines)
            std::cout << render_inline(l) << "\n";
        return;
    }

    size_t num_cols = table.headers.size();

    std::vector<int> col_widths(num_cols, 0);
    for (size_t i = 0; i < num_cols; ++i)
        col_widths[i] = display_width(table.headers[i]);
    for (const auto& row : table.rows) {
        for (size_t i = 0; i < num_cols && i < row.size(); ++i) {
            int w = display_width(row[i]);
            if (w > col_widths[i]) col_widths[i] = w;
        }
    }

    auto make_border = [&](const char* left, const char* mid, const char* right) {
        std::cout << ansi::GRAY << left;
        for (size_t i = 0; i < num_cols; ++i) {
            if (i > 0) std::cout << mid;
            int w = col_widths[i] + 2;
            for (int j = 0; j < w; ++j)
                std::cout << box::H;
        }
        std::cout << right << ansi::RESET << "\n";
    };

    auto make_content = [&](const std::vector<std::string>& cells,
                            const std::string& fg) {
        std::cout << ansi::GRAY << box::V << ansi::RESET;
        for (size_t i = 0; i < num_cols; ++i) {
            const std::string& cell = (i < cells.size()) ? cells[i] : "";
            int w = display_width(cell);
            int pad = col_widths[i] - w;
            TableAlign align = (i < table.alignments.size())
                               ? table.alignments[i] : TableAlign::Default;

            std::cout << fg;
            std::cout << " ";

            switch (align) {
                case TableAlign::Right:
                    for (int j = 0; j < pad; ++j) std::cout << " ";
                    std::cout << cell;
                    break;
                case TableAlign::Center: {
                    int left = pad / 2;
                    int right = pad - left;
                    for (int j = 0; j < left; ++j) std::cout << " ";
                    std::cout << cell;
                    for (int j = 0; j < right; ++j) std::cout << " ";
                    break;
                }
                default:
                    std::cout << cell;
                    for (int j = 0; j < pad; ++j) std::cout << " ";
                    break;
            }

            std::cout << " ";
            std::cout << ansi::RESET;
            std::cout << ansi::GRAY << box::V << ansi::RESET;
        }
        std::cout << "\n";
    };

    make_border(box::TL, box::LT, box::TR);
    make_content(table.headers, ansi::BOLD + std::string(ansi::WHITE));
    make_border(box::LV, box::X, box::RV);
    for (const auto& row : table.rows) {
        make_content(row, ansi::GRAY);
    }
    make_border(box::BL, box::RT, box::BR);
}

// ============================================================================
// 主函数
// ============================================================================

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    std::string md_path = (argc > 1) ? argv[1] : "demo.md";

    std::ifstream file(md_path);
    if (!file) {
        std::string alt_path = "example/example_markdown/demo.md";
        file.open(alt_path);
        if (!file) {
            std::cerr << "Cannot open: " << md_path << "\n";
            std::cerr << "Usage: example_markdown [markdown_file]\n";
            return 1;
        }
    }

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(file, line)) {
        lines.push_back(line);
    }
    file.close();

    bool in_code_block = false;
    std::string code_lang;
    std::vector<std::string> code_lines;
    TableBuffer table_buf;
    bool in_table = false;

    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& ln = lines[i];

        if (in_code_block) {
            if (ln.size() >= 3 && ln[0] == '`' && ln[1] == '`' && ln[2] == '`') {
                std::cout << render_code_block(code_lang, code_lines);
                in_code_block = false;
                code_lang.clear();
                code_lines.clear();
            } else {
                code_lines.push_back(ln);
            }
            continue;
        }

        if (ln.size() >= 3 && ln[0] == '`' && ln[1] == '`' && ln[2] == '`') {
            if (in_table) {
                render_table_colored(table_buf.lines());
                table_buf.clear();
                in_table = false;
            }
            in_code_block = true;
            if (ln.size() > 3) {
                auto sv = std::string_view(ln).substr(3);
                while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t'))
                    sv.remove_prefix(1);
                code_lang = std::string(sv);
            } else {
                code_lang.clear();
            }
            code_lines.clear();
            continue;
        }

        if (is_table_row(ln) || in_table) {
            if (table_buf.feed_line(ln)) {
                in_table = true;
                continue;
            } else {
                render_table_colored(table_buf.lines());
                table_buf.clear();
                in_table = false;
            }
        }

        if (!ln.empty() && ln[0] == '#') {
            int level = 0;
            while (level < static_cast<int>(ln.size()) && ln[level] == '#')
                level++;
            if (level >= 1 && level <= 6) {
                auto sv = std::string_view(ln).substr(level);
                while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t'))
                    sv.remove_prefix(1);
                std::cout << render_heading(level, sv);
                continue;
            }
        }

        if (is_horizontal_rule(ln)) {
            std::cout << render_hr(60);
            continue;
        }

        if (is_list_item(ln)) {
            std::cout << render_list_item(ln);
            continue;
        }

        if (ln.empty()) {
            std::cout << "\n";
            continue;
        }
        {
            bool all_space = true;
            for (char c : ln) {
                if (c != ' ' && c != '\t' && c != '\r') { all_space = false; break; }
            }
            if (all_space) {
                std::cout << "\n";
                continue;
            }
        }

        std::cout << render_inline(ln) << "\n";
    }

    if (in_code_block) {
        std::cout << render_code_block(code_lang, code_lines);
    }
    if (in_table) {
        render_table_colored(table_buf.lines());
    }

    return 0;
}
