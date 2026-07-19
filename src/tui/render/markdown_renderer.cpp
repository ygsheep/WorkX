/**
 * @file markdown_renderer.cpp
 * @brief Markdown 表格解析与渲染实现
 */

#include "tui/render/markdown_renderer.h"
#include "tui/utils/utf8_utils.h"
#include <algorithm>
#include <cctype>
#include <format>
#include <numeric>
#include <sstream>

namespace agent {

// ---- helpers ----

static std::string_view trim_sv(std::string_view sv) {
    while (!sv.empty() && (sv.front() == ' ' || sv.front() == '\t' || sv.front() == '\r'))
        sv.remove_prefix(1);
    while (!sv.empty() && (sv.back() == ' ' || sv.back() == '\t' || sv.back() == '\r'))
        sv.remove_suffix(1);
    return sv;
}

// ---- is_table_row ----

bool is_table_row(std::string_view line) {
    auto trimmed = trim_sv(line);
    return !trimmed.empty() && trimmed.front() == '|';
}

// ---- is_table_separator ----

bool is_table_separator(std::string_view line, std::vector<TableAlign>& alignments) {
    alignments.clear();
    auto trimmed = trim_sv(line);
    if (trimmed.empty() || trimmed.front() != '|') return false;

    // split_table_row 保证至少返回一个 cell（尾部 push_back），无需 empty 检查
    auto cells = split_table_row(trimmed);

    for (const auto& cell : cells) {
        // Trim and check colon position (start = left, end = right)
        auto c = trim_sv(cell);
        if (c.empty()) return false;
        bool starts_colon = (c.front() == ':');
        bool ends_colon = (c.back() == ':');
        bool has_dash = false;

        for (size_t i = 0; i < c.size(); ++i) {
            char ch = c[i];
            if (ch == '-') {
                has_dash = true;
            } else if (ch == ':' || ch == ' ' || ch == '\t') {
                // colons and spaces allowed
            } else {
                return false;  // invalid character
            }
        }

        if (!has_dash) return false;  // must have at least one dash

        if (starts_colon && ends_colon) {
            alignments.push_back(TableAlign::Center);
        } else if (starts_colon) {
            alignments.push_back(TableAlign::Left);
        } else if (ends_colon) {
            alignments.push_back(TableAlign::Right);
        } else {
            alignments.push_back(TableAlign::Default);
        }
    }
    return true;
}

// ---- split_table_row ----

std::vector<std::string> split_table_row(std::string_view line) {
    std::vector<std::string> cells;
    auto trimmed = trim_sv(line);

    // Remove leading | if present
    if (!trimmed.empty() && trimmed.front() == '|') {
        trimmed.remove_prefix(1);
    }
    // Remove trailing | if present
    if (!trimmed.empty() && trimmed.back() == '|') {
        trimmed.remove_suffix(1);
    }

    // Split by unescaped |
    std::string current;
    for (size_t i = 0; i < trimmed.size(); ++i) {
        char c = trimmed[i];
        if (c == '\\' && i + 1 < trimmed.size() && trimmed[i + 1] == '|') {
            // Escaped pipe → literal |
            current += '|';
            ++i;
        } else if (c == '|') {
            cells.push_back(std::string(trim_sv(current)));
            current.clear();
        } else {
            current += c;
        }
    }
    cells.push_back(std::string(trim_sv(current)));

    return cells;
}

// ---- parse_table ----

MarkdownTable parse_table(const std::vector<std::string>& lines) {
    MarkdownTable result;
    if (lines.size() < 2) return result;

    // Line 0: headers
    result.headers = split_table_row(lines[0]);

    // Line 1: separator (must be valid)
    if (!is_table_separator(lines[1], result.alignments)) {
        return result;
    }

    // Validate alignment count matches headers
    if (result.alignments.size() < result.headers.size()) {
        result.alignments.resize(result.headers.size(), TableAlign::Default);
    }

    // Lines 2+: data rows
    for (size_t i = 2; i < lines.size(); ++i) {
        auto row_cells = split_table_row(lines[i]);
        // Pad to match header column count
        if (row_cells.size() < result.headers.size()) {
            row_cells.resize(result.headers.size(), "");
        }
        result.rows.push_back(std::move(row_cells));
    }

    result.valid = true;
    return result;
}

// ---- render_table ----

// Box-drawing byte sequences (UTF-8)
static constexpr const char* BOX_TL = "\xe2\x94\x8c";  // ┌
static constexpr const char* BOX_TR = "\xe2\x94\x90";  // ┐
static constexpr const char* BOX_BL = "\xe2\x94\x94";  // └
static constexpr const char* BOX_BR = "\xe2\x94\x98";  // ┘
static constexpr const char* BOX_H  = "\xe2\x94\x80";  // ─
static constexpr const char* BOX_V  = "\xe2\x94\x82";  // │
static constexpr const char* BOX_LT = "\xe2\x94\xac";  // ┬
static constexpr const char* BOX_RT = "\xe2\x94\xb4";  // ┴
static constexpr const char* BOX_LV = "\xe2\x94\x9c";  // ├
static constexpr const char* BOX_RV = "\xe2\x94\xa4";  // ┤
static constexpr const char* BOX_X  = "\xe2\x94\xbc";  // ┼

static std::string strip_ansi(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        if (i + 1 < text.size() && text[i] == '\x1b' && text[i + 1] == '[') {
            i += 2;
            while (i < text.size() && !(text[i] >= 0x40 && text[i] <= 0x7e))
                i++;
            if (i < text.size()) i++;
        } else {
            result += text[i];
            i++;
        }
    }
    return result;
}

static int cell_visible_width(std::string_view text) {
    std::string rendered = render_inline(text);
    std::string plain = strip_ansi(rendered);
    return display_width(plain);
}

static std::string make_line(const int width) {
    std::string s;
    for (int i = 0; i < width; ++i) s += BOX_H;
    return s;
}

static std::string pad_cell(const std::string& text, int col_width, TableAlign align) {
    std::string rendered = render_inline(text);
    std::string plain = strip_ansi(rendered);
    int text_w = display_width(plain);

    if (text_w > col_width) {
        plain = truncate_to_width(plain, col_width);
        text_w = display_width(plain);
        rendered = plain;
    }
    int padding = col_width - text_w;
    std::string result;
    switch (align) {
        case TableAlign::Right:
            result = std::string(padding, ' ') + rendered;
            break;
        case TableAlign::Center: {
            int left = padding / 2;
            int right = padding - left;
            result = std::string(left, ' ') + rendered + std::string(right, ' ');
            break;
        }
        default:  // Default and Left
            result = rendered + std::string(padding, ' ');
            break;
    }
    // Add 1-space padding on each side
    return std::string(" ") + result + std::string(" ");
}

static std::string make_border(const std::vector<int>& col_widths,
                               const char* left, const char* mid, const char* right) {
    std::string s = left;
    for (size_t i = 0; i < col_widths.size(); ++i) {
        if (i > 0) s += mid;
        s += make_line(col_widths[i] + 2);  // +2 for the 1-space padding on each side
    }
    s += right;
    s += "\n";
    return s;
}

static std::string make_row(const std::vector<std::string>& cells,
                            const std::vector<int>& col_widths,
                            const std::vector<TableAlign>& aligns) {
    std::string s = BOX_V;
    for (size_t i = 0; i < col_widths.size(); ++i) {
        TableAlign a = (i < aligns.size()) ? aligns[i] : TableAlign::Default;
        s += pad_cell(i < cells.size() ? cells[i] : "", col_widths[i], a);
        s += BOX_V;
    }
    s += "\n";
    return s;
}

std::string render_table(const MarkdownTable& table, int max_width) {
    if (!table.valid || table.headers.empty()) return "";

    const size_t num_cols = table.headers.size();

    // Calculate natural column widths (based on visible width after inline rendering)
    std::vector<int> col_widths(num_cols, 0);
    for (size_t i = 0; i < num_cols; ++i) {
        col_widths[i] = cell_visible_width(table.headers[i]);
    }
    for (const auto& row : table.rows) {
        for (size_t i = 0; i < num_cols && i < row.size(); ++i) {
            if (const int w = cell_visible_width(row[i]); w > col_widths[i]) col_widths[i] = w;
        }
    }

    // Calculate total width (borders + padding + content)
    // Each column: | + space + content + space → col_width + 2 + 1 (for border)
    // Total = num_cols * (col_width + 3) + 1 (rightmost border)
    auto calc_total = [&]() {
        return 1 + std::accumulate(col_widths.begin(), col_widths.end(), 0,
            [](int acc, int w) { return acc + w + 3; });  // border + space + content + space
    };

    // Reduce column widths if exceeding max_width
    if (max_width > 0) {
        // Minimum: each column needs at least 3 display columns (… + 2 padding + border)
        const int min_col = 3;
        while (calc_total() > max_width) {
            // Find widest column
            int max_idx = 0;
            for (size_t i = 1; i < num_cols; ++i) {
                if (col_widths[i] > col_widths[max_idx]) max_idx = static_cast<int>(i);
            }
            if (col_widths[max_idx] <= min_col) break;
            col_widths[max_idx]--;
        }
    }

    std::string output;

    // Top border: ┌───┬───┐
    output += make_border(col_widths, BOX_TL, BOX_LT, BOX_TR);

    // Header row: │ H1 │ H2 │
    output += make_row(table.headers, col_widths, table.alignments);

    // Header separator: ├───┼───┤
    output += make_border(col_widths, BOX_LV, BOX_X, BOX_RV);

    // Data rows: │ a │ b │
    for (const auto& row : table.rows) {
        output += make_row(row, col_widths, table.alignments);
    }

    // Bottom border: └───┴───┘
    output += make_border(col_widths, BOX_BL, BOX_RT, BOX_BR);

    return output;
}

// ---- TableBuffer ----

bool TableBuffer::feed_line(const std::string& line) {
    switch (m_state) {
        case State::Empty:
            if (is_table_row(line)) {
                m_lines.push_back(line);
                m_state = State::PendingSeparator;
                return true;
            }
            return false;

        case State::PendingSeparator: {
            // Check if this line is a valid separator
            std::vector<TableAlign> aligns;
            if (is_table_separator(line, aligns)) {
                m_lines.push_back(line);
                m_state = State::Collecting;
                return true;
            }
            // Not a separator → invalid table
            m_state = State::Invalid;
            return false;
        }

        case State::Collecting:
            if (is_table_row(line)) {
                m_lines.push_back(line);
                return true;
            }
            // Non-table line ends the table
            m_state = State::Complete;
            return false;

        default:
            return false;
    }
}

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
    constexpr auto GRAY      = "\x1b[90m";
    constexpr auto WHITE     = "\x1b[97m";
}

// ============================================================================
// 扩展 box-drawing 字符
// ============================================================================

static constexpr const char* BOX_DH   = "\xe2\x95\x90";  // ═
static constexpr const char* BULLET   = "\xe2\x80\xa2";  // •

// ============================================================================
// 辅助函数
// ============================================================================

static std::string trim_str(const std::string& s) {
    size_t start = 0, end = s.size();
    while (start < end && (s[start] == ' ' || s[start] == '\t' || s[start] == '\r' || s[start] == '\n'))
        start++;
    while (end > start && (s[end-1] == ' ' || s[end-1] == '\t' || s[end-1] == '\r' || s[end-1] == '\n'))
        end--;
    return s.substr(start, end - start);
}

// ============================================================================
// 行内强调解析
// ============================================================================

std::string render_inline(std::string_view text) {
    std::string result;
    size_t i = 0;

    while (i < text.size()) {
        if (text[i] == '\\' && i + 1 < text.size()) {
            char next = text[i + 1];
            if (next == '`' || next == '*' || next == '_' || next == '~' ||
                next == '#' || next == '[' || next == ']' || next == '\\' ||
                next == '|' || next == '!' || next == '(' || next == ')') {
                result += next;
                i += 2;
                continue;
            }
        }

        if (text[i] == '`') {
            size_t end = text.find('`', i + 1);
            if (end != std::string_view::npos) {
                result += ansi::GRAY;
                result += std::string(text.substr(i + 1, end - i - 1));
                result += ansi::RESET;
                i = end + 1;
                continue;
            }
        }

        if (i + 2 < text.size() && text[i] == '*' && text[i+1] == '*' && text[i+2] == '*') {
            size_t end = text.find("***", i + 3);
            if (end != std::string_view::npos) {
                result += ansi::BOLD;
                result += ansi::ITALIC;
                result += render_inline(text.substr(i + 3, end - i - 3));
                result += ansi::RESET;
                i = end + 3;
                continue;
            }
        }

        if (i + 1 < text.size() && text[i] == '*' && text[i+1] == '*') {
            size_t end = text.find("**", i + 2);
            if (end != std::string_view::npos) {
                result += ansi::YELLOW;
                result += render_inline(text.substr(i + 2, end - i - 2));
                result += ansi::RESET;
                i = end + 2;
                continue;
            }
        }

        if (text[i] == '*') {
            size_t end = text.find('*', i + 1);
            if (end != std::string_view::npos && end > i + 1) {
                result += ansi::ITALIC;
                result += render_inline(text.substr(i + 1, end - i - 1));
                result += ansi::RESET;
                i = end + 1;
                continue;
            }
        }

        if (i + 1 < text.size() && text[i] == '~' && text[i+1] == '~') {
            size_t end = text.find("~~", i + 2);
            if (end != std::string_view::npos) {
                result += ansi::STRIKE;
                result += render_inline(text.substr(i + 2, end - i - 2));
                result += ansi::RESET;
                i = end + 2;
                continue;
            }
        }

        result += text[i];
        i++;
    }
    return result;
}

// ============================================================================
// 标题渲染
// ============================================================================

std::string render_heading(int level, std::string_view text) {
    std::ostringstream os;

    std::string indent;
    if (level >= 6)      indent = "    ";
    else if (level == 5) indent = "  ";

    os << "\n" << indent;

    if (level <= 3) {
        os << ansi::BOLD;
    } else if (level >= 5) {
        os << ansi::DIM;
    }

    os << render_inline(text) << ansi::RESET << "\n";

    if (level <= 2) {
        int w = display_width(text);
        os << indent << ansi::DIM;
        const char* line_ch = (level == 1) ? BOX_DH : BOX_H;
        for (int i = 0; i < w; ++i)
            os << line_ch;
        os << ansi::RESET << "\n";
    }

    return os.str();
}

// ============================================================================
// 分隔线渲染
// ============================================================================

bool is_horizontal_rule(std::string_view line) {
    std::string t = trim_str(std::string(line));
    if (t.size() < 3) return false;
    char c = t[0];
    if (c != '-' && c != '*' && c != '_') return false;
    return std::all_of(t.begin(), t.end(),
        [c](char ch) { return ch == c || ch == ' '; });
}

std::string render_hr(int width) {
    std::ostringstream os;
    os << ansi::GRAY;
    for (int i = 0; i < width; ++i)
        os << BOX_H;
    os << ansi::RESET << "\n";
    return os.str();
}

// ============================================================================
// 列表渲染
// ============================================================================

bool is_list_item(std::string_view line) {
    std::string t = std::string(line);
    while (!t.empty() && (t[0] == ' ' || t[0] == '\t'))
        t = t.substr(1);
    if (t.empty()) return false;

    if (t.size() >= 2 && (t[0] == '-' || t[0] == '*' || t[0] == '+') && t[1] == ' ')
        return true;

    size_t dot = t.find('.');
    if (dot > 0 && dot != std::string::npos && dot + 1 < t.size() && t[dot + 1] == ' ') {
        return std::all_of(t.begin(), t.begin() + dot,
            [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)); });
    }
    return false;
}

std::string render_list_item(std::string_view line) {
    std::ostringstream os;

    int indent = 0;
    while (indent < static_cast<int>(line.size()) && line[indent] == ' ')
        indent++;

    std::string content = std::string(line.substr(indent));
    std::string indent_str(indent, ' ');

    if (content.size() >= 2 &&
        (content[0] == '-' || content[0] == '*' || content[0] == '+') &&
        content[1] == ' ') {
        os << indent_str << ansi::CYAN << BULLET << " " << ansi::RESET
           << render_inline(content.substr(2)) << "\n";
    } else {
        size_t dot = content.find('.');
        std::string num = content.substr(0, dot + 2);
        std::string rest = content.substr(dot + 2);
        os << indent_str << ansi::YELLOW << num << ansi::RESET
           << render_inline(rest) << "\n";
    }

    return os.str();
}

// ============================================================================
// 代码块渲染
// ============================================================================

std::string render_code_block(std::string_view lang,
                               const std::vector<std::string>& lines,
                               int max_width) {
    (void)lang;  // 不显示语言标签
    std::ostringstream os;

    // 无边框样式：黑底白字 + 行号，无 → 符号
    constexpr const char* BG_BLACK = "\x1b[40;37m";  // 黑底白字

    // 计算行号宽度
    int line_count = static_cast<int>(lines.size());
    int line_w = 1;
    for (int n = line_count; n >= 10; n /= 10) ++line_w;

    // 渲染每行：行号 + 空格 + 内容，整行黑底白字，填充到终端宽度
    for (int i = 0; i < line_count; ++i) {
        os << BG_BLACK;
        std::string prefix = std::format(" {:>{}}  ", i + 1, line_w);
        os << prefix << lines[i];
        // 填充空格到终端宽度，让背景色覆盖整行
        if (max_width > 0) {
            int used = display_width(prefix) + display_width(lines[i]);
            int pad = max_width - used;
            for (int j = 0; j < pad; ++j) os << " ";
        }
        os << ansi::RESET << "\n";
    }

    return os.str();
}

} // namespace workx
