/**
 * @file markdown_renderer.cpp
 * @brief Markdown 表格解析与渲染实现
 */

#include "tui/render/markdown_renderer.h"
#include "tui/render/syntax_highlighter.h"
#include "tui/utils/utf8_utils.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <format>
#include <numeric>
#include <sstream>

namespace tui {

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

// strip_ansi 复用 syntax_highlighter.h 中的声明（非 static，外部链接）
// 原 markdown_renderer.cpp 内的 static strip_ansi 触发 C4211 warning（extern 重定义为 static）

/// @brief 单元格预渲染结果（#1 缓存优化）
/// @details render_table 预处理所有单元格一次，make_row 直接取用，
///          避免原 pad_cell + cell_visible_width 双重渲染（每个单元格被
///          render_inline / strip_ansi / display_width 各调用 2 次）
struct CellInfo {
    std::string rendered;  ///< render_inline 后的带 ANSI 文本
    std::string plain;     ///< strip_ansi 后的纯文本
    int width;             ///< display_width(plain)
};

/// @brief 预渲染单元格：一次调用 render_inline + strip_ansi + display_width
static CellInfo render_cell(const std::string& text) {
    CellInfo info;
    info.rendered = render_inline(text);
    info.plain = strip_ansi(info.rendered);
    info.width = display_width(info.plain);
    return info;
}

static std::string make_line(const int width) {
    // BOX_H 是 3 字节 UTF-8（─ = E2 94 80），预分配容量避免多次扩容
    // 直接用 std::string(count, char) 重复字节模式不适用（多字节字符），
    // 改为 reserve + 循环 append，整体只触发 1 次堆分配
    std::string s;
    s.reserve(static_cast<size_t>(width) * 3);
    for (int i = 0; i < width; ++i) s += BOX_H;
    return s;
}

/// @brief 用预渲染的 CellInfo 填充单元格，避免重复调用 render_inline
static std::string pad_cell_cached(const CellInfo& info, int col_width, TableAlign align) {
    std::string rendered = info.rendered;
    std::string plain = info.plain;
    int text_w = info.width;

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

// 保留原 pad_cell 接口供外部调用（如 flush_table_as_text 不需要，但保持 ABI 兼容）
// 注：make_row（原 cells + pad_cell 版本）已删除，render_table 统一用 make_row_cached

static std::string make_border(const std::vector<int>& col_widths,
                               const char* left, const char* mid, const char* right) {
    // 预估容量：sum(col_width + 2) * 3 字节 + 边框字符
    // 避免多次 += 触发多次堆分配
    size_t total_bytes = std::strlen(left) + std::strlen(right) + 1;  // +1 for \n
    for (size_t i = 0; i < col_widths.size(); ++i) {
        total_bytes += static_cast<size_t>(col_widths[i] + 2) * 3;  // BOX_H 3 字节
        if (i > 0) total_bytes += std::strlen(mid);
    }
    std::string s;
    s.reserve(total_bytes);
    s = left;
    for (size_t i = 0; i < col_widths.size(); ++i) {
        if (i > 0) s += mid;
        s += make_line(col_widths[i] + 2);  // +2 for the 1-space padding on each side
    }
    s += right;
    s += "\n";
    return s;
}

/// @brief 用预渲染的 CellInfo 数组构建行（#1 缓存优化）
/// @details 原实现每行每列都重新 render_inline + strip_ansi + display_width，
///          现在直接复用 render_table 预处理的 cell_infos，避免重复计算
static std::string make_row_cached(const std::vector<CellInfo>& cells,
                                   const std::vector<int>& col_widths,
                                   const std::vector<TableAlign>& aligns) {
    std::string s = BOX_V;
    for (size_t i = 0; i < col_widths.size(); ++i) {
        TableAlign a = (i < aligns.size()) ? aligns[i] : TableAlign::Default;
        s += pad_cell_cached(i < cells.size() ? cells[i] : CellInfo{}, col_widths[i], a);
        s += BOX_V;
    }
    s += "\n";
    return s;
}

std::string render_table(const MarkdownTable& table, int max_width, int indent) {
    if (!table.valid || table.headers.empty()) return "";

    // E.4：缩进占用可用宽度，嵌套在引用块/列表中的表格按缩进后的宽度计算列宽
    if (max_width > 0 && indent > 0) {
        max_width -= indent;
        if (max_width < 1) max_width = 1;
    }

    const size_t num_cols = table.headers.size();

    // #1 缓存优化：一次性预渲染所有单元格，后续 make_row / col_widths 计算复用
    // 原实现每格 render_inline 调用 2 次（cell_visible_width 1 次 + pad_cell 1 次），
    // 现在每格只调用 1 次，对 N 行 × M 列表格减少 N×M 次 render_inline + strip_ansi + display_width
    std::vector<CellInfo> header_infos(num_cols);
    for (size_t i = 0; i < num_cols; ++i) {
        header_infos[i] = render_cell(table.headers[i]);
    }

    std::vector<std::vector<CellInfo>> row_infos(table.rows.size());
    for (size_t r = 0; r < table.rows.size(); ++r) {
        const auto& row = table.rows[r];
        row_infos[r].resize(num_cols);
        for (size_t i = 0; i < num_cols && i < row.size(); ++i) {
            row_infos[r][i] = render_cell(row[i]);
        }
    }

    // Calculate natural column widths (复用缓存，无需重新计算)
    std::vector<int> col_widths(num_cols, 0);
    for (size_t i = 0; i < num_cols; ++i) {
        col_widths[i] = header_infos[i].width;
    }
    for (const auto& row_info : row_infos) {
        for (size_t i = 0; i < num_cols && i < row_info.size(); ++i) {
            if (row_info[i].width > col_widths[i]) col_widths[i] = row_info[i].width;
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

    // #3 reserve 优化：预估输出总大小，避免多次 += 触发多次堆分配
    // 估算：3 条边框 + (1 + rows) 行数据，每行约 sum(col_width + 3) * 3 字节
    const size_t estimated_total = (table.rows.size() + 4) *  // 行数
        (1 + std::accumulate(col_widths.begin(), col_widths.end(), size_t{0},
            [](size_t acc, int w) { return acc + static_cast<size_t>(w + 3) * 3; }));
    std::string output;
    output.reserve(estimated_total);

    // Top border: ┌───┬───┐
    output += make_border(col_widths, BOX_TL, BOX_LT, BOX_TR);

    // Header row: │ H1 │ H2 │（复用预渲染缓存）
    output += make_row_cached(header_infos, col_widths, table.alignments);

    // Header separator: ├───┼───┤
    output += make_border(col_widths, BOX_LV, BOX_X, BOX_RV);

    // Data rows: │ a │ b │（复用预渲染缓存）
    for (const auto& row_info : row_infos) {
        output += make_row_cached(row_info, col_widths, table.alignments);
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
            // E.2：允许表头与分隔行之间存在空行（LLM 输出常见格式）
            // 空行不消费 m_lines，状态保持 PendingSeparator，等待真正的分隔行
            auto trimmed = trim_sv(line);
            if (trimmed.empty()) {
                return true;  // 消费空行，但不加入 m_lines
            }
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

        case State::Invalid:
            // E.2：Invalid 状态下若已缓存表头，保留 m_lines 供调用方尝试渲染
            // （调用方可在 is_invalid() && !lines().empty() 时尝试渲染最小表格）
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

    // 性能短路：若文本不含任何 markdown 标记字符，直接返回原文本
    // 避免对纯 ASCII / 纯文本单元格做无谓的逐字符循环 + 字符串构建
    // 标记字符：` * ~ \  （其他如 # [ ] ! ( ) | 仅在块级语法生效，行内无需处理）
    {
        bool has_marker = false;
        for (size_t k = 0; k < text.size(); ++k) {
            char c = text[k];
            if (c == '`' || c == '*' || c == '~' || c == '\\') {
                has_marker = true;
                break;
            }
        }
        if (!has_marker) {
            return std::string(text);
        }
    }

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
                result += ansi::BOLD;
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
    if (dot != std::string::npos && dot + 1 < t.size() && t[dot + 1] == ' ') {
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
    (void)lang;       // 不显示语言标签
    (void)max_width;  // 当前实现未按宽度截断
    std::ostringstream os;

    // 1. 整块交给语法高亮器（按 lang 选 grammar，未知 lang 原样返回）
    //    每行自包含 ANSI，不会跨行泄漏颜色，可安全按 \n split 后逐行渲染
    std::string joined;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) joined.push_back('\n');
        joined += lines[i];
    }
    std::string highlighted = highlight_code(lang, joined);

    // 2. 按 \n 重新拆成行（高亮后行数应与原始一致）
    std::vector<std::string> hl_lines;
    {
        std::string cur;
        for (char c : highlighted) {
            if (c == '\n') { hl_lines.push_back(std::move(cur)); cur.clear(); }
            else cur.push_back(c);
        }
        hl_lines.push_back(std::move(cur));
        // 防御：若行数不一致（不应发生），回退到原始 lines
        if (hl_lines.size() != lines.size()) {
            hl_lines = lines;
        }
    }

    // 3. 计算行号宽度（按总行数位数，最少 1 位）
    //    硬约束：行号右对齐，行号与内容之间恰好两个空格
    const int total_lines = static_cast<int>(hl_lines.size());
    int line_num_width = 1;
    for (int n = total_lines; n >= 10; n /= 10) ++line_num_width;

    // 4. 顶部语言标签（无 ┌─┐ 框线，仅文本）
    if (!lang.empty()) {
        os << ansi::GRAY << lang << ansi::RESET << "\n";
    }

    // 5. 渲染每行：│ + 行号(右对齐, 灰) + 两空格 + 内容(保留语法高亮)
    //    硬约束：左侧 │（U+2502），无右边框、无顶/底框线
    const std::string box_v = "\xe2\x94\x82";  // │ U+2502
    for (int i = 0; i < total_lines; ++i) {
        const std::string& l = hl_lines[i];
        std::string num_str = std::to_string(i + 1);
        std::string num_padding;
        if (static_cast<int>(num_str.size()) < line_num_width) {
            num_padding.append(line_num_width - num_str.size(), ' ');
        }

        os << ansi::GRAY << box_v << ansi::RESET
           << ansi::GRAY << num_padding << num_str << ansi::RESET
           << "  "
           << l
           << "\n";
    }

    return os.str();
}

// ============================================================================
// render_markdown_block — 整段 Markdown 块级渲染
// ============================================================================

std::string render_markdown_block(std::string_view text) {
    if (text.empty()) return {};

    // 按行拆分
    std::vector<std::string> lines;
    {
        std::string cur;
        for (char c : text) {
            if (c == '\n') { lines.push_back(std::move(cur)); cur.clear(); }
            else cur.push_back(c);
        }
        lines.push_back(std::move(cur));
    }

    std::ostringstream os;
    size_t i = 0;
    bool in_code_block = false;
    std::string code_lang;
    std::vector<std::string> code_lines;

    while (i < lines.size()) {
        const std::string& line = lines[i];

        // ---- 代码块检测 ----
        if (line.size() >= 3 && line.substr(0, 3) == "```") {
            if (!in_code_block) {
                in_code_block = true;
                code_lang.clear();
                // 提取 lang（``` 后的内容，trim）
                if (line.size() > 3) {
                    std::string lang = line.substr(3);
                    size_t b = 0, e = lang.size();
                    while (b < e && (lang[b] == ' ' || lang[b] == '\t')) ++b;
                    while (e > b && (lang[e-1] == ' ' || lang[e-1] == '\t')) --e;
                    code_lang = lang.substr(b, e - b);
                }
                ++i;
                continue;
            } else {
                // 结束代码块
                if (!code_lines.empty() || !code_lang.empty()) {
                    os << render_code_block(code_lang, code_lines);
                }
                in_code_block = false;
                code_lang.clear();
                code_lines.clear();
                ++i;
                continue;
            }
        }

        if (in_code_block) {
            code_lines.push_back(line);
            ++i;
            continue;
        }

        // ---- 空行 ----
        {
            std::string t = line;
            size_t b = 0, e = t.size();
            while (b < e && (t[b] == ' ' || t[b] == '\t')) ++b;
            if (b == e) {
                os << "\n";
                ++i;
                continue;
            }
        }

        // ---- 标题 ----
        if (!line.empty() && line[0] == '#') {
            int level = 0;
            while (level < static_cast<int>(line.size()) && line[level] == '#') ++level;
            if (level >= 1 && level <= 6
                && level < static_cast<int>(line.size()) && line[level] == ' ') {
                // trim_start 后的文本
                std::string rest = line.substr(level);
                size_t rb = 0;
                while (rb < rest.size() && (rest[rb] == ' ' || rest[rb] == '\t')) ++rb;
                os << render_heading(level, rest.substr(rb));
                ++i;
                continue;
            }
        }

        // ---- 分隔线 ----
        if (is_horizontal_rule(line)) {
            os << render_hr();
            ++i;
            continue;
        }

        // ---- 列表项 ----
        if (is_list_item(line)) {
            os << render_list_item(line);
            ++i;
            continue;
        }

        // ---- 表格行（单行 | 开头，简单渲染为行内；完整表格需多行缓冲，这里简化）----
        // 完整表格渲染需要多行缓冲，块级渲染场景下保持简单：按普通文本处理
        // （流式场景下 OutputFormatter 已处理完整表格）

        // ---- 普通文本行 ----
        os << render_inline(line) << "\n";
        ++i;
    }

    // 末尾未闭合的代码块（防御）
    if (in_code_block && !code_lines.empty()) {
        os << render_code_block(code_lang, code_lines);
    }

    return os.str();
}

} // namespace tui
