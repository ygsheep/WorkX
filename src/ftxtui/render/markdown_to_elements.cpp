#include "render/markdown_to_elements.h"
#include "render/syntax_highlight.h"
#include "theme/icons.h"
#include "theme/strings.h"
#include "theme/theme.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/string.hpp>

namespace ftxtui {

using ftxui::Element;
using ftxui::Elements;
using ftxui::Color;
using ftxui::BorderStyle;

namespace {

/// @brief 去首尾空白（空格/制表符），返回副本
std::string trim_copy(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return std::string(s.substr(b, e - b));
}

struct StyledSpan {
    std::string text;
    bool bold = false;
    bool italic = false;
    bool strike = false;
    bool code = false;
    Color fg;  // valid 由 fg_valid 表示
    bool fg_valid = false;
};

// 用户消息块配色：深色背景 + 左侧 accent 竖线
const Color kUserMsgBg = theme::T::Panel;
const Color kUserAccent = theme::T::Accent;

// 折叠卡片（思考/工具）圆角边框颜色
const Color kCardBorder = theme::T::TextFaint;

// 思考动画帧：Braille 旋转字符（复制自 src/tui/status_bar.cpp SPINNER_FRAMES）
constexpr const char* kSpinnerFrames[] = {
    "\u2813", "\u2819", "\u2839", "\u2838", "\u283C",
    "\u2834", "\u2826", "\u2827", "\u2807", "\u280F",
};
constexpr int kSpinnerFrameCount = 10;

/// @brief 进行中动画色：HSV 橙黄渐变（与 src/tui get_animated_color 同色相曲线）
Color spinner_color(std::size_t frame) {
    return Color::HSV(static_cast<uint8_t>(20 + (frame * 5) % 26), 255, 255);
}

/// @brief 从工具调用参数 JSON 提取文件路径（read_file/write_file/edit_file 的
///        file_path、grep 的 path），供工具卡展开首行展示
std::string tool_file_path(std::string_view args_json) {
    if (args_json.empty()) return {};
    try {
        const auto j = nlohmann::json::parse(args_json);
        for (const char* key : {"file_path", "path"}) {
            const auto it = j.find(key);
            if (it != j.end() && it->is_string())
                return it->get<std::string>();
        }
    } catch (const nlohmann::json::exception&) {
        // 非 JSON 参数：无路径行
    }
    return {};
}

// ============================================================================
// 工具结果特化渲染辅助（对齐 src/tui render_code_tool_result 的解析逻辑）
// ============================================================================

/// @brief 文件路径扩展名 → ftxtui 支持的语法高亮语言标签；未知返回空
std::string lang_from_path(const std::string& path) {
    if (path.empty()) return {};
    const size_t slash = path.find_last_of("/\\");
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (name == "CMakeLists.txt") return "cpp";
    if (name == "Dockerfile") return "bash";
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) return {};
    std::string ext = name.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == "c" || ext == "h") return "c";
    if (ext == "cc" || ext == "cpp" || ext == "cxx" || ext == "hpp" || ext == "hxx"
        || ext == "ino") return "cpp";
    if (ext == "py" || ext == "pyw" || ext == "pyi") return "python";
    if (ext == "js" || ext == "jsx" || ext == "mjs" || ext == "cjs") return "js";
    if (ext == "ts" || ext == "tsx") return "ts";
    if (ext == "rs") return "rust";
    if (ext == "go") return "go";
    if (ext == "sh" || ext == "bash" || ext == "zsh" || ext == "ksh") return "bash";
    if (ext == "json") return "json";
    if (ext == "yaml" || ext == "yml") return "yaml";
    if (ext == "sql") return "sql";
    return {};
}

/// @brief 拆分 FileRead 行号行："  123→content" → 前缀 + 代码
void split_fileread_line(const std::string& line,
                         std::string& line_prefix, std::string& code_part) {
    line_prefix.clear();
    code_part = line;
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    const size_t num_start = i;
    while (i < line.size() && line[i] >= '0' && line[i] <= '9') ++i;
    if (i == num_start) return;  // 无数字，不是行号格式
    if (i + 3 > line.size()) return;
    // 紧跟着 → (U+2192, UTF-8: E2 86 92)
    if (static_cast<unsigned char>(line[i]) != 0xE2
        || static_cast<unsigned char>(line[i + 1]) != 0x86
        || static_cast<unsigned char>(line[i + 2]) != 0x92) return;
    const size_t arrow_end = i + 3;
    line_prefix = line.substr(0, arrow_end);
    code_part = line.substr(arrow_end);
}

/// @brief 从 FileRead 行号前缀提取数字行号；无法解析返回 -1
int extract_fileread_line_number(const std::string& prefix) {
    size_t i = 0;
    while (i < prefix.size() && (prefix[i] == ' ' || prefix[i] == '\t')) ++i;
    if (i >= prefix.size() || prefix[i] < '0' || prefix[i] > '9') return -1;
    int num = 0;
    while (i < prefix.size() && prefix[i] >= '0' && prefix[i] <= '9') {
        num = num * 10 + (prefix[i] - '0');
        ++i;
    }
    return num;
}

/// @brief │N  行号前缀渲染宽度（按最大行号位数，最少 1）
int calc_line_num_width(int max_line_num) {
    if (max_line_num < 1) max_line_num = 1;
    int width = 1;
    for (int n = max_line_num; n >= 10; n /= 10) ++width;
    return width;
}

/// @brief FileRead 末尾元数据行（对齐 file_read_tool.cpp 实际格式）：
///        "(read lines N-M, more lines available)" / "(read lines N-M, total T, truncated)"
bool is_fileread_metadata_line(const std::string& line) {
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    return line.compare(i, 12, "(read lines ") == 0;
}

/// @brief diff 行前缀类型
enum class DiffPrefix : int { Add, Del, Context, None };
struct DiffLine {
    DiffPrefix prefix = DiffPrefix::Context;
    std::string content;
};

/// @brief 解析 diff 文本：跳过 ---/+++/@@ 头，识别 +/-/空格 前缀
std::vector<DiffLine> parse_diff_lines(std::string_view diff) {
    std::vector<DiffLine> lines;
    size_t pos = 0;
    while (pos <= diff.size()) {
        const size_t nl = diff.find('\n', pos);
        const std::string_view line = (nl == std::string_view::npos)
            ? diff.substr(pos)
            : diff.substr(pos, nl - pos);
        const bool header =
            (line.size() >= 3 && (line.substr(0, 3) == "---" || line.substr(0, 3) == "+++"))
            || (line.size() >= 2 && line.substr(0, 2) == "@@");
        if (!header && !line.empty()) {
            DiffLine dl;
            if (line[0] == '+') {
                dl.prefix = DiffPrefix::Add;
                dl.content = std::string(line.substr(1));
            } else if (line[0] == '-') {
                dl.prefix = DiffPrefix::Del;
                dl.content = std::string(line.substr(1));
            } else if (line[0] == ' ') {
                dl.content = std::string(line.substr(1));
            } else {
                dl.prefix = DiffPrefix::None;
                dl.content = std::string(line);
            }
            lines.push_back(std::move(dl));
        } else if (line.empty()) {
            lines.push_back({});
        }
        if (nl == std::string_view::npos) break;
        pos = nl + 1;
    }
    return lines;
}

/// @brief │N  行号前缀（Dim 色，右对齐，尾随两空格）
Element line_num_prefix(int line_num, int num_width) {
    const std::string box_v = "\u2502";
    const std::string num_str = std::to_string(line_num);
    const int pad = std::max(0, num_width - static_cast<int>(num_str.size()));
    return ftxui::color(theme::T::TextDim)(
        ftxui::text(box_v + std::string(pad, ' ') + num_str + "  "));
}

/// @brief 工具结果展开的最大显示行数（对齐 src/tui MAX_DISPLAY_LINES）
constexpr int kMaxToolResultLines = 60;

/// @brief 用户消息装饰器：深色背景（#1e1e1e）+ 左侧 accent 竖线（无边框盒）
/// @details 先渲染子节点再铺背景/边框，避免覆盖文字前景色
class UserMessageBox : public ftxui::Node {
   public:
    explicit UserMessageBox(Element child) : Node(Elements{std::move(child)}) {}

    void ComputeRequirement() override {
        children_[0]->ComputeRequirement();
        requirement_ = children_[0]->requirement();
        requirement_.min_x += 1;  // 左边框列
    }

    void SetBox(ftxui::Box box) override {
        box_ = box;
        children_[0]->SetBox(ftxui::Box{box.x_min + 1, box.x_max, box.y_min, box.y_max});
    }

    void Render(ftxui::Screen& screen) override {
        children_[0]->Render(screen);
        for (int y = box_.y_min; y <= box_.y_max; ++y) {
            for (int x = box_.x_min + 1; x <= box_.x_max; ++x) {
                screen.PixelAt(x, y).background_color = kUserMsgBg;
            }
            auto& p = screen.PixelAt(box_.x_min, y);
            p.character = "\u2502";  // │
            p.foreground_color = kUserAccent;
            p.background_color = kUserMsgBg;
        }
    }
};

// ---- 行内解析：把 `**bold**`, `*it*`, `~~del~~`, `` `code` `` 拆为 StyledSpan 序列 ----
std::vector<StyledSpan> parse_inline_spans(std::string_view text) {
    std::vector<StyledSpan> out;
    std::string cur;
    auto flush = [&]() {
        if (!cur.empty()) {
            StyledSpan s;
            s.text = std::move(cur);
            s.code = true;
            cur.clear();
            out.push_back(std::move(s));
        }
    };
    size_t i = 0;
    while (i < text.size()) {
        // 代码：`...`
        if (text[i] == '`') {
            size_t end = text.find('`', i + 1);
            if (end != std::string_view::npos) {
                StyledSpan s;
                s.text.assign(text.substr(i + 1, end - i - 1));
                s.code = true;
                out.push_back(std::move(s));
                i = end + 1;
                continue;
            }
        }
        if (i + 2 < text.size() && text[i] == '*' && text[i+1] == '*' && text[i+2] == '*') {
            size_t end = text.find("***", i + 3);
            if (end != std::string_view::npos) {
                // 粗斜体：拆分内层
                StyledSpan s;
                s.text.assign(text.substr(i + 3, end - i - 3));
                s.bold = true;
                s.italic = true;
                out.push_back(std::move(s));
                i = end + 3;
                continue;
            }
        }
        if (i + 1 < text.size() && text[i] == '*' && text[i+1] == '*') {
            size_t end = text.find("**", i + 2);
            if (end != std::string_view::npos && end > i + 1) {
                StyledSpan s;
                s.text.assign(text.substr(i + 2, end - i - 2));
                s.bold = true;
                out.push_back(std::move(s));
                i = end + 2;
                continue;
            }
        }
        if (i + 1 < text.size() && text[i] == '~' && text[i+1] == '~') {
            size_t end = text.find("~~", i + 2);
            if (end != std::string_view::npos) {
                StyledSpan s;
                s.text.assign(text.substr(i + 2, end - i - 2));
                s.strike = true;
                out.push_back(std::move(s));
                i = end + 2;
                continue;
            }
        }
        if (text[i] == '*') {
            size_t end = text.find('*', i + 1);
            if (end != std::string_view::npos && end > i + 1) {
                StyledSpan s;
                s.text.assign(text.substr(i + 1, end - i - 1));
                s.italic = true;
                out.push_back(std::move(s));
                i = end + 1;
                continue;
            }
        }
        // 转义
        if (text[i] == '\\' && i + 1 < text.size() &&
            (text[i+1] == '*' || text[i+1] == '_' || text[i+1] == '`' || text[i+1] == '~')) {
            cur.push_back(text[i + 1]);
            i += 2;
            continue;
        }
        cur.push_back(text[i]);
        ++i;
    }
    // 收尾普通文本（非 code）
    if (!cur.empty()) {
        StyledSpan s;
        s.text = std::move(cur);
        out.push_back(std::move(s));
    }
    if (out.empty()) {
        StyledSpan s;
        s.text = "";
        out.push_back(std::move(s));  // 保证至少一个 span，段落可为空行
    }
    return out;
}

Element span_element(const StyledSpan& s) {
    Element e = ftxui::text(s.text);
    if (s.bold) e = e | ftxui::bold;
    if (s.italic) e = e | ftxui::color(theme::T::Text);
    if (s.strike) e = e | ftxui::strikethrough;
    if (s.fg_valid) e = e | ftxui::color(s.fg);
    if (s.code) e = e | ftxui::color(theme::T::Text);
    return e;
}

Element line_to_element(std::string_view line) {
    auto spans = parse_inline_spans(line);
    Elements children;
    children.reserve(spans.size());
    for (const auto& s : spans) children.push_back(span_element(s));
    return ftxui::hbox(std::move(children));
}

bool is_code_fence(std::string_view line, std::string& lang_out) {
    if (line.size() < 3 || line.substr(0, 3) != "```") return false;
    lang_out.assign(line.substr(3));
    // trim
    while (!lang_out.empty() && (lang_out.front() == ' ' || lang_out.front() == '\t'))
        lang_out.erase(lang_out.begin());
    while (!lang_out.empty() && std::isspace(static_cast<unsigned char>(lang_out.back())))
        lang_out.pop_back();
    return true;
}

bool is_hr(std::string_view line) {
    std::string t(line.begin(), line.end());
    while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.erase(t.begin());
    if (t.size() < 3) return false;
    char c = t[0];
    if (c != '-' && c != '*' && c != '_') return false;
    for (char ch : t)
        if (ch != c && ch != ' ') return false;
    return true;
}

bool is_list_item(std::string_view line, std::string& content_out, bool& ordered_out) {
    std::string t(line.begin(), line.end());
    size_t indent = 0;
    while (indent < t.size() && (t[indent] == ' ' || t[indent] == '\t')) ++indent;
    t = t.substr(indent);
    if (t.empty()) return false;
    // 无序 - / * / +
    if (t.size() >= 2 && (t[0] == '-' || t[0] == '*' || t[0] == '+') && t[1] == ' ') {
        content_out = t.substr(2);
        ordered_out = false;
        return true;
    }
    // 有序 N.
    size_t dot = t.find('.');
    if (dot != std::string::npos && dot + 1 < t.size() && t[dot + 1] == ' ') {
        bool digits = true;
        for (size_t k = 0; k < dot; ++k)
            if (!std::isdigit(static_cast<unsigned char>(t[k]))) { digits = false; break; }
        if (digits) {
            content_out = t.substr(dot + 2);
            ordered_out = true;
            return true;
        }
    }
    return false;
}

bool is_heading(std::string_view line, int& level_out, std::string_view& content_out) {
    size_t n = 0;
    while (n < line.size() && line[n] == '#') ++n;
    if (n < 1 || n > 6 || n >= line.size() || line[n] != ' ') return false;
    level_out = static_cast<int>(n);
    content_out = line.substr(n + 1);
    return true;
}

bool is_table_row(std::string_view line) {
    auto t = line;
    while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.remove_prefix(1);
    return !t.empty() && t.front() == '|';
}

/// @brief 拆分表格行（| a | b | → [a, b]；支持 \| 转义与首尾竖线）
std::vector<std::string> split_table_row(std::string_view line);

/// @brief 表格分隔行（| --- | :---: | 等），并解析各列对齐（0=左 1=中 2=右）
bool is_table_separator(std::string_view line, std::vector<int>& aligns_out) {
    aligns_out.clear();
    auto t = line;
    while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.remove_prefix(1);
    if (t.empty() || t.front() != '|') return false;

    auto cells = split_table_row(t);
    for (const auto& cell : cells) {
        std::string c = cell;
        while (!c.empty() && (c.front() == ' ' || c.front() == '\t')) c.erase(c.begin());
        while (!c.empty() && (c.back() == ' ' || c.back() == '\t')) c.pop_back();
        if (c.empty()) return false;
        bool starts_colon = c.front() == ':';
        bool ends_colon = c.back() == ':';
        bool has_dash = false;
        for (char ch : c) {
            if (ch == '-') has_dash = true;
            else if (ch != ':' && ch != ' ' && ch != '\t') return false;
        }
        if (!has_dash) return false;
        aligns_out.push_back(starts_colon && ends_colon ? 1
                                    : starts_colon ? 0
                                    : ends_colon ? 2 : 0);
    }
    return true;
}

/// @brief 拆分表格行（| a | b | → [a, b]；支持 \| 转义与首尾竖线）
std::vector<std::string> split_table_row(std::string_view line) {
    std::vector<std::string> cells;
    auto t = line;
    while (!t.empty() && (t.front() == ' ' || t.front() == '\t')) t.remove_prefix(1);
    if (!t.empty() && t.front() == '|') t.remove_prefix(1);
    if (!t.empty() && t.back() == '|') t.remove_suffix(1);

    std::string cur;
    for (size_t i = 0; i < t.size(); ++i) {
        char c = t[i];
        if (c == '\\' && i + 1 < t.size() && t[i + 1] == '|') {
            cur += '|';
            ++i;
        } else if (c == '|') {
            cells.push_back(trim_copy(cur));
            cur.clear();
        } else {
            cur += c;
        }
    }
    cells.push_back(trim_copy(cur));
    return cells;
}

/// @brief 表格块渲染（FTXUI 版，样式对齐 src/tui render_table）
/// @param rows 原始表格行：rows[0]=表头，rows[1]=分隔行，rows[2..]=数据行
Element render_table_block(const std::vector<std::string>& rows) {
    const std::vector<std::string> header = split_table_row(rows[0]);
    std::vector<int> aligns;
    is_table_separator(rows[1], aligns);
    aligns.resize(header.size(), 0);

    std::vector<std::vector<std::string>> data;
    for (size_t i = 2; i < rows.size(); ++i)
        data.push_back(split_table_row(rows[i]));

    size_t cols = header.size();
    for (const auto& r : data) cols = std::max(cols, r.size());

    std::vector<int> widths(cols, 0);
    for (size_t c = 0; c < header.size(); ++c)
        widths[c] = std::max(widths[c], ftxui::string_width(header[c]));
    for (const auto& r : data)
        for (size_t c = 0; c < r.size(); ++c)
            widths[c] = std::max(widths[c], ftxui::string_width(r[c]));

    auto border = [&](const char* left, const char* mid, const char* right) {
        std::string s = left;
        for (size_t c = 0; c < cols; ++c) {
            if (c > 0) s += mid;
            for (int k = 0; k < widths[c] + 2; ++k) s += "\u2500";
        }
        s += right;
        return ftxui::text(s);
    };
    auto row_elem = [&](const std::vector<std::string>& cells, bool head) {
        Elements children;
        children.push_back(ftxui::text("\u2502 "));
        for (size_t c = 0; c < cols; ++c) {
            if (c > 0) children.push_back(ftxui::text(" \u2502 "));
            std::string cell = c < cells.size() ? cells[c] : "";
            int w = ftxui::string_width(cell);
            int pad = std::max(0, widths[c] - w);
            int left = 0, right = pad;
            if (aligns[c] == 1) { left = pad / 2; right = pad - left; }
            else if (aligns[c] == 2) { left = pad; right = 0; }
            // 单元格走行内解析（对齐 src/tui：表格内容里的 **粗体** 也渲染）
            auto e = line_to_element(std::string(left, ' ') + cell + std::string(right, ' '));
            if (head) e = e | ftxui::bold;
            children.push_back(std::move(e));
        }
        children.push_back(ftxui::text(" \u2502"));
        return ftxui::hbox(std::move(children));
    };

    Elements rows_elem;
    rows_elem.push_back(border("\u250c", "\u252c", "\u2510"));  // ┌ ┬ ┐
    rows_elem.push_back(row_elem(header, true));
    rows_elem.push_back(border("\u251c", "\u253c", "\u2524"));  // ├ ┼ ┤
    for (const auto& r : data) rows_elem.push_back(row_elem(r, false));
    rows_elem.push_back(border("\u2514", "\u2534", "\u2518"));  // └ ┴ ┘
    return ftxui::vbox(std::move(rows_elem));
}

/// @brief 按 '\n' 拆行（与 build_markdown 的行拆分一致）
std::vector<std::string> split_lines(std::string_view text) {
    std::vector<std::string> lines;
    std::string cur;
    for (char c : text) {
        if (c == '\n') { lines.push_back(std::move(cur)); cur.clear(); }
        else cur.push_back(c);
    }
    lines.push_back(std::move(cur));
    return lines;
}

/// @brief 统计 Markdown 块级渲染行数（A3：与 build_markdown 布局一一对应）
/// @details 与渲染约定的对应关系：
///          - 空行 → emptyElement（min_y=0，不占行）
///          - 标题/分隔线/列表/表格/段落 → 文本 1 行
///          - 代码块 → 每代码行 1 行 + 语言标签 1 行（仅当有内容时渲染）
int count_markdown_lines(const std::vector<std::string>& lines) {
    int h = 0;
    bool in_code = false;
    std::string code_lang;
    std::vector<std::string> code_lines;

    auto flush_code = [&]() {
        if (in_code && !code_lines.empty()) {
            h += static_cast<int>(code_lines.size()) + 2;  // 上下留白各 1 行（vPad）
            if (!code_lang.empty()) ++h;  // 语言标签行
        }
        in_code = false;
        code_lang.clear();
        code_lines.clear();
    };

    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        if (line.size() >= 3 && line.substr(0, 3) == "```") {
            if (!in_code) {
                in_code = true;
                (void)is_code_fence(line, code_lang);
                continue;
            }
            flush_code();
            continue;
        }
        if (in_code) { code_lines.push_back(line); continue; }
        // 表格块：表头行 + 分隔行 + 连续 | 行 → 顶/表头/中/底边框 4 行 + 数据行
        std::vector<int> aligns_tmp;
        if (is_table_row(line) && i + 1 < lines.size() &&
            is_table_separator(lines[i + 1], aligns_tmp)) {
            size_t j = i + 2;
            while (j < lines.size() && is_table_row(lines[j])) ++j;
            h += 4 + static_cast<int>(j - i - 2);
            i = j - 1;
            continue;
        }
        // 空行/全空白：emptyElement（min_y=0）不占行；其余块各 1 行
        bool blank = line.empty() ||
            std::all_of(line.begin(), line.end(),
                        [](char c) { return c == ' ' || c == '\t'; });
        if (!blank) ++h;
    }
    flush_code();
    return h;
}

}  // namespace

Element build_markdown(std::string_view text, int width) {
    (void)width;
    if (text.empty()) return ftxui::text("");

    const std::vector<std::string> lines = split_lines(text);

    Elements blocks;
    bool in_code = false;
    std::string code_lang;
    std::vector<std::string> code_lines;

    auto flush_code = [&]() {
        if (in_code && !code_lines.empty()) {
            // 代码块：深色背景块（#1e1e1e），无边框线；行内 flex 铺满消息宽度
            Elements code_rows;
            code_rows.reserve(code_lines.size());
            for (const auto& cl : code_lines) {
                code_rows.push_back(ftxui::hbox({
                    ftxui::text("  "),
                    ftxui::flex(highlight_code_line(cl, code_lang)),
                }));
            }
            auto code_elem = ftxui::vbox(std::move(code_rows));
            if (!code_lang.empty()) {
                auto lang_elem = ftxui::hbox({
                    ftxui::text("  "),
                    ftxui::flex(ftxui::color(theme::T::Text)(ftxui::text(code_lang))),
                });
                code_elem = ftxui::vbox({lang_elem, code_elem});
            }
            // 代码块统一上下留白（README「代码上下一行距离」），Panel 背景
            blocks.push_back(theme::vPad(code_elem | ftxui::bgcolor(theme::T::Panel)));
        }
        in_code = false;
        code_lang.clear();
        code_lines.clear();
    };

    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string& line = lines[i];
        if (line.size() >= 3 && line.substr(0, 3) == "```") {
            if (!in_code) {
                in_code = true;
                (void)is_code_fence(line, code_lang);
                continue;
            } else {
                flush_code();
                continue;
            }
        }
        if (in_code) { code_lines.push_back(line); continue; }

        // 空行
        if (line.empty() ||
            std::all_of(line.begin(), line.end(),
                        [](char c) { return c == ' ' || c == '\t'; })) {
            blocks.push_back(ftxui::emptyElement());
            continue;
        }

        int level = 0;
        std::string_view heading;
        if (is_heading(line, level, heading)) {
            auto e = line_to_element(heading) | ftxui::bold;
            if (level >= 5) e = e | ftxui::color(theme::T::Text);
            blocks.push_back(e);
            continue;
        }

        if (is_hr(line)) {
            blocks.push_back(ftxui::separator());
            continue;
        }

        std::string content;
        bool ordered = false;
        if (is_list_item(line, content, ordered)) {
            (void)ordered;
            auto inner = line_to_element(content);
            blocks.push_back(ftxui::hbox({ftxui::text("  • "), ftxui::flex(inner)}));
            continue;
        }

        // 表格块：表头行 + 分隔行 + 连续 | 行（样式对齐 src/tui render_table）
        std::vector<int> aligns_tmp;
        if (is_table_row(line) && i + 1 < lines.size() &&
            is_table_separator(lines[i + 1], aligns_tmp)) {
            std::vector<std::string> tbl;
            tbl.push_back(line);
            tbl.push_back(lines[++i]);  // 分隔行
            while (i + 1 < lines.size() && is_table_row(lines[i + 1]))
                tbl.push_back(lines[++i]);
            blocks.push_back(render_table_block(tbl));
            continue;
        }

        // 孤立 | 行（无分隔行）：降级为普通行内文本（保留 | 分隔，可读）
        if (is_table_row(line)) {
            blocks.push_back(ftxui::color(theme::T::Text)(line_to_element(line)));
            continue;
        }

        // 普通段落
        blocks.push_back(ftxui::flex(line_to_element(line)));
    }
    flush_code();
    return ftxui::vbox(std::move(blocks));
}

Element build_inline_line(std::string_view line) {
    return ftxui::flex(line_to_element(line));
}

/// @brief 渲染工具结果内容（对齐 src/tui render_code_tool_result）：
///   - Read：状态行 + 代码高亮 + │N 行号 + 元数据行 + 截断
///   - Write/Edit：状态文本（Dim）+ diff（前景高亮 + +/- 背景色 + │N 序号）+ 截断
///   - 其他工具/错误：通用 markdown（原行为）
/// @details 代码与 diff 区域统一 Panel 背景 + 上下留白（theme::vPad），
///          与 build_markdown 代码块视觉一致（README「代码上下一行距离」）。
Element render_tool_result(const ToolCallNode& t, int width) {
    if (t.is_error) return build_markdown(t.result, width);
    const std::string fpath = tool_file_path(t.arguments);
    const bool is_read = t.tool_name == "Read" || t.tool_name == "FileRead";
    const bool is_write_or_edit = t.tool_name == "Write" || t.tool_name == "Edit"
                               || t.tool_name == "FileWrite" || t.tool_name == "FileEdit";
    if (!is_read && !is_write_or_edit) return build_markdown(t.result, width);

    const std::string lang = lang_from_path(fpath);
    const std::string arrow = "\u23bf";  // ⎿
    const std::string indent4 = "    ";
    Elements rows;

    // ---- 分支 A: Write/Edit — 拆分状态文本 + diff ----
    if (is_write_or_edit) {
        size_t pos = 0, line_start = 0, diff_start = std::string::npos;
        while (pos <= t.result.size()) {
            if (pos == t.result.size() || t.result[pos] == '\n') {
                if (pos - line_start >= 4 && t.result.compare(line_start, 4, "--- ") == 0) {
                    diff_start = line_start;
                    break;
                }
                line_start = pos + 1;
            }
            ++pos;
        }
        if (diff_start == std::string::npos) {
            // 无 diff（create 模式 / 无变化）：退回通用渲染
            return build_markdown(t.result, width);
        }

        // 状态文本（Dim 色，⎿ 前缀；对齐 src/tui 分支 A）
        std::string status_text = t.result.substr(0, diff_start);
        while (!status_text.empty() && status_text.back() == '\n') status_text.pop_back();
        std::string line;
        auto flush_status = [&]() {
            if (!line.empty()) {
                rows.push_back(ftxui::hbox({
                    ftxui::text(indent4 + arrow + " "),
                    ftxui::color(theme::T::TextDim)(ftxui::text(line)),
                }));
                line.clear();
            }
        };
        for (const char c : status_text) {
            if (c == '\n') flush_status();
            else line.push_back(c);
        }
        flush_status();

        // diff 块：Panel 背景 + 上下留白 + │N 序号 + 前景高亮 + +/- 背景色
        std::vector<DiffLine> diff = parse_diff_lines(t.result.substr(diff_start));
        const bool truncated = static_cast<int>(diff.size()) > kMaxToolResultLines;
        if (truncated) diff.resize(kMaxToolResultLines);
        const int num_width = calc_line_num_width(static_cast<int>(diff.size()));
        Elements code_rows;
        code_rows.reserve(diff.size());
        for (size_t i = 0; i < diff.size(); ++i) {
            const DiffLine& dl = diff[i];
            Element content = highlight_code_line(dl.content, lang);
            if (dl.prefix == DiffPrefix::Add) {
                content = content | ftxui::bgcolor(Color::RGB(0x00, 0x5f, 0x00));
            } else if (dl.prefix == DiffPrefix::Del) {
                content = content | ftxui::bgcolor(Color::RGB(0x5f, 0x00, 0x00));
            }
            code_rows.push_back(ftxui::hbox({
                ftxui::text(indent4),
                line_num_prefix(static_cast<int>(i + 1), num_width),
                ftxui::flex(content),
            }));
        }
        if (truncated) {
            code_rows.push_back(ftxui::hbox({
                ftxui::text(indent4 + arrow + " "),
                ftxui::color(theme::T::TextDim)(ftxui::text(
                    std::format("(... truncated, showing first {} lines)", kMaxToolResultLines))),
            }));
        }
        rows.push_back(theme::vPad(
            ftxui::vbox(std::move(code_rows)) | ftxui::bgcolor(theme::T::Panel)));
        return ftxui::vbox(std::move(rows));
    }

    // ---- 分支 B: FileRead — 状态行 + 剥离行号 + 末尾元数据 ----
    rows.push_back(ftxui::hbox({
        ftxui::text(indent4 + arrow + " "),
        ftxui::color(theme::T::TextDim)(ftxui::text(
            "The file " + (fpath.empty() ? std::string{"file"} : fpath)
            + " has been read successfully.")),
    }));

    std::vector<std::string> lines;
    {
        std::string cur;
        for (const char c : t.result) {
            if (c == '\n') { lines.push_back(std::move(cur)); cur.clear(); }
            else cur.push_back(c);
        }
        lines.push_back(std::move(cur));
    }
    // 剥离末尾元数据行（(read lines ...) 及其前的空行）
    std::vector<std::string> meta_lines;
    while (!lines.empty()) {
        const std::string& last = lines.back();
        if (last.empty() || is_fileread_metadata_line(last)) {
            meta_lines.insert(meta_lines.begin(), last);
            lines.pop_back();
        } else {
            break;
        }
    }
    // 截断（代码部分，不含元数据）
    const bool truncated = static_cast<int>(lines.size()) > kMaxToolResultLines;
    if (truncated) lines.resize(kMaxToolResultLines);

    // 剥离行号前缀 + 提取实际行号
    std::vector<int> line_nums;
    line_nums.reserve(lines.size());
    int max_line_num = 0;
    std::vector<std::string> code_lines;
    code_lines.reserve(lines.size());
    for (const auto& l : lines) {
        std::string prefix, code;
        split_fileread_line(l, prefix, code);
        const int n = extract_fileread_line_number(prefix);
        line_nums.push_back(n);
        max_line_num = std::max(max_line_num, n);
        code_lines.push_back(std::move(code));
    }
    const int num_width = calc_line_num_width(max_line_num);
    Elements code_rows;
    code_rows.reserve(code_lines.size());
    for (size_t i = 0; i < code_lines.size(); ++i) {
        Elements row;
        row.push_back(ftxui::text(indent4));
        if (line_nums[i] > 0) row.push_back(line_num_prefix(line_nums[i], num_width));
        else row.push_back(ftxui::text(""));
        row.push_back(ftxui::flex(highlight_code_line(code_lines[i], lang)));
        code_rows.push_back(ftxui::hbox(std::move(row)));
    }
    if (truncated) {
        code_rows.push_back(ftxui::hbox({
            ftxui::text(indent4 + arrow + " "),
            ftxui::color(theme::T::TextDim)(ftxui::text(
                std::format("(... truncated, showing first {} lines)", kMaxToolResultLines))),
        }));
    }
    rows.push_back(theme::vPad(
        ftxui::vbox(std::move(code_rows)) | ftxui::bgcolor(theme::T::Panel)));

    // 元数据行（Dim，不高亮）
    for (const auto& m : meta_lines) {
        if (m.empty()) {
            rows.push_back(ftxui::text(""));
        } else {
            rows.push_back(ftxui::hbox({
                ftxui::text(indent4 + arrow + " "),
                ftxui::color(theme::T::TextDim)(ftxui::text(m)),
            }));
        }
    }
    return ftxui::vbox(std::move(rows));
}

/// @brief 工具结果展开行数估算（与 render_tool_result 布局逐行对齐；A3 单一布局源）
int estimate_tool_result_lines(const ToolCallNode& t) {
    const std::string fpath = tool_file_path(t.arguments);
    const bool is_read = t.tool_name == "Read" || t.tool_name == "FileRead";
    const bool is_write_or_edit = t.tool_name == "Write" || t.tool_name == "Edit"
                               || t.tool_name == "FileWrite" || t.tool_name == "FileEdit";
    if (t.is_error || (!is_read && !is_write_or_edit))
        return std::max(1, estimate_markdown_height(t.result));

    if (is_write_or_edit) {
        size_t pos = 0, line_start = 0, diff_start = std::string::npos;
        while (pos <= t.result.size()) {
            if (pos == t.result.size() || t.result[pos] == '\n') {
                if (pos - line_start >= 4 && t.result.compare(line_start, 4, "--- ") == 0) {
                    diff_start = line_start;
                    break;
                }
                line_start = pos + 1;
            }
            ++pos;
        }
        if (diff_start == std::string::npos)
            return std::max(1, estimate_markdown_height(t.result));

        int status_lines = 0;
        bool in_line = false;
        for (size_t i = 0; i < diff_start; ++i) {
            if (t.result[i] == '\n') { if (in_line) ++status_lines; in_line = false; }
            else in_line = true;
        }
        if (in_line) ++status_lines;

        const int diff_lines = static_cast<int>(
            parse_diff_lines(std::string_view(t.result).substr(diff_start)).size());
        int h = status_lines + std::min(diff_lines, kMaxToolResultLines) + 2;  // vPad
        if (diff_lines > kMaxToolResultLines) ++h;  // 截断提示行
        return std::max(1, h);
    }

    // Read：状态行 1 + 代码行（截断）+ vPad 2 + 元数据 + 截断提示
    int code_lines = 0, meta_lines = 0;
    {
        std::vector<std::string> lines;
        std::string cur;
        for (const char c : t.result) {
            if (c == '\n') { lines.push_back(std::move(cur)); cur.clear(); }
            else cur.push_back(c);
        }
        lines.push_back(std::move(cur));
        while (!lines.empty()) {
            const std::string& last = lines.back();
            if (last.empty() || is_fileread_metadata_line(last)) {
                ++meta_lines;
                lines.pop_back();
            } else {
                break;
            }
        }
        code_lines = static_cast<int>(lines.size());
    }
    int h = 1 + std::min(code_lines, kMaxToolResultLines) + 2 + meta_lines;
    if (code_lines > kMaxToolResultLines) ++h;  // 截断提示行
    return std::max(1, h);
}

int estimate_markdown_height(std::string_view text) {
    // build_markdown 入口对空输入返回 text("")（FTXUI 空文本 min_y=1）
    if (text.empty()) return 1;
    return count_markdown_lines(split_lines(text));
}

int estimate_message_height(const MessageNode& msg) {
    // 与 build_message 的视觉结构逐行对齐（A3 单一布局源）：
    // - 用户块：上/下留白各 1 行 + markdown 内容
    // - 思考/工具卡：圆角边框 2 行 + 头行 1 行 + 展开内容
    // - 流式游标 1 行；正文行恒 ≥1 行（左侧缩进 text("  ") 占 1 行）
    auto content_lines = [](std::string_view t) {
        return std::max(1, estimate_markdown_height(t));
    };
    if (msg.role == MsgRole::User) {
        int h = 2;  // 顶部/底部留白各 1 行
        if (!msg.text.empty() || msg.streaming)
            h += content_lines(msg.text);
        return h;
    }

    int h = 0;
    if (msg.role == MsgRole::Error) ++h;  // "✖ 错误" 头
    if (msg.reasoned && !msg.reasoning.empty()) {
        h += 3;  // 边框 2 行 + 头行 1 行
        if (msg.reasoning_expanded)
            h += content_lines(msg.reasoning);
    }
    if (!msg.text.empty() || msg.streaming)
        h += content_lines(msg.text);
    for (const auto& t : msg.tool_calls) {
        h += 3;  // 边框 2 行 + 头行 1 行
        if (t.done && t.expanded)
            h += estimate_tool_result_lines(t) + (tool_file_path(t.arguments).empty() ? 0 : 1);
    }
    if (msg.streaming) ++h;  // 流式游标
    return h;
}

Element build_context_gauge(int used, int limit) {
    if (limit <= 0) return ftxui::text(std::string(str::kGaugeNA));
    double ratio = std::clamp(static_cast<double>(used) / limit, 0.0, 1.0);
    return ftxui::gaugeRight(static_cast<float>(ratio));
}

Element build_message(const MessageNode& msg, int width, std::size_t anim_frame,
                      std::deque<CardHit>* card_hits) {
    // 用户消息：深色背景块 + 左边框线，无角色头，内容左缩进 2 格、上下留白各 1 行
    if (msg.role == MsgRole::User) {
        Elements body;
        body.push_back(ftxui::text(" "));  // 顶部间距
        if (!msg.text.empty() || msg.streaming) {
            body.push_back(ftxui::hbox({ftxui::text("  "),
                                        ftxui::flex(build_markdown(msg.text, width))}));
        }
        body.push_back(ftxui::text(" "));  // 底部间距
        return std::make_shared<UserMessageBox>(ftxui::vbox(std::move(body)));
    }

    Elements rows;

    if (msg.role == MsgRole::Error) {
        rows.push_back(ftxui::hbox({
            ftxui::text(std::string(str::kErrorHeader)) | ftxui::color(Color::RedLight) | ftxui::bold,
            ftxui::separatorEmpty(),
        }));
    }

    // 思考折叠（reasoning）：圆角卡片，头行可点击展开/收起
    if (msg.reasoned && !msg.reasoning.empty()) {
        Element thinking_header;
        if (!msg.sealed) {
            // 运行中：Braille 旋转动画 + 橙黄渐变（src/tui 同款）
            const char* spin = kSpinnerFrames[anim_frame % kSpinnerFrameCount];
            Color c = spinner_color(anim_frame);
            thinking_header = ftxui::hbox({
                ftxui::text(spin) | ftxui::color(c),
                ftxui::text(std::string(str::kThinkingRunning)) | ftxui::color(c),
            });
        } else {
            std::string sec = std::format("{:.1f}s", msg.reasoning_ms / 1000.0);
            // Nerd Font（不支持时 ASCII 降级）：灯泡=思考，chevron=折叠指示
            const std::string chevron(msg.reasoning_expanded
                                          ? theme::icon_chevron_down()
                                          : theme::icon_chevron_right());
            thinking_header = ftxui::hbox({
                ftxui::text(std::string(theme::icon_think()) + std::string(str::kThinkingLabel)),
                ftxui::text(sec),
                ftxui::text("  "),
                ftxui::text(chevron),
            });
        }
        Elements card_rows;
        card_rows.push_back(ftxui::hbox({
            ftxui::text("  "),
            thinking_header,
        }));
        if (msg.reasoning_expanded) {
            card_rows.push_back(ftxui::hbox({
                ftxui::text("    "),
                ftxui::flex(ftxui::color(theme::T::Text)(build_markdown(msg.reasoning, width)))
            }));
        }
        auto card = ftxui::borderStyled(BorderStyle::ROUNDED,
                                        msg.sealed ? kCardBorder : spinner_color(anim_frame))(
            ftxui::vbox(std::move(card_rows)));
        if (card_hits) {
            card_hits->push_back(CardHit{});
            card_hits->back().tool_idx = -1;
            card = card | ftxui::reflect(card_hits->back().box);
        }
        rows.push_back(std::move(card));
    }

    // 正文
    if (!msg.text.empty() || msg.streaming) {
        rows.push_back(ftxui::hbox({ftxui::text("  "),
                                    ftxui::flex(build_markdown(msg.text, width))}));
    }

    // 工具块：圆角卡片，头行可点击展开/收起
    for (std::size_t ti = 0; ti < msg.tool_calls.size(); ++ti) {
        const auto& t = msg.tool_calls[ti];
        std::string status;
        Color c = Color::YellowLight;
        if (t.running) {
            // 运行中：Braille 旋转动画 + 橙黄渐变（src/tui 同款）
            status = std::string(kSpinnerFrames[anim_frame % kSpinnerFrameCount]) + " " + t.tool_name;
            c = spinner_color(anim_frame);
        }
        else if (t.is_error) { status = "✖ " + t.tool_name; c = Color::RedLight; }
        else { status = "✓ " + t.tool_name; c = Color::GreenLight; }

        Elements card_rows;
        card_rows.push_back(ftxui::hbox({
            ftxui::text("  "),
            // 工具图标（Nerd Font 不支持时降级为空），chevron=折叠指示
            ftxui::text(std::string(theme::icon_tool()) + " "),
            ftxui::color(c)(ftxui::text(status)),
            t.done ? ftxui::text("  ") : ftxui::text(""),
            t.done ? ftxui::text(std::string(t.expanded
                                                 ? theme::icon_chevron_down()
                                                 : theme::icon_chevron_right()))
                   : ftxui::text(""),
        }));
        if (t.done && t.expanded) {
            Elements content;
            const std::string fpath = tool_file_path(t.arguments);
            if (!fpath.empty()) {
                content.push_back(ftxui::hbox({
                    ftxui::text("    "),
                    ftxui::flex(ftxui::color(theme::T::TextDim)(ftxui::text(fpath))),
                }));
            }
            content.push_back(ftxui::hbox({
                ftxui::text("    "),
                ftxui::flex(render_tool_result(t, width))
            }));
            card_rows.push_back(ftxui::vbox(std::move(content)));
        }
        auto card = ftxui::borderStyled(BorderStyle::ROUNDED,
                                        t.running ? spinner_color(anim_frame) : kCardBorder)(
            ftxui::vbox(std::move(card_rows)));
        if (card_hits) {
            card_hits->push_back(CardHit{});
            card_hits->back().tool_idx = static_cast<int>(ti);
            card = card | ftxui::reflect(card_hits->back().box);
        }
        rows.push_back(std::move(card));
    }

    // 流式游标
    if (msg.streaming) {
        rows.push_back(ftxui::hbox({ftxui::text("  "), ftxui::text("▋") | ftxui::color(theme::T::TextFaint)}));
    }

    return ftxui::vbox(std::move(rows));
}

}  // namespace ftxtui