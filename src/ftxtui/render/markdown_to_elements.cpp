#include "render/markdown_to_elements.h"
#include "render/syntax_highlight.h"
#include "theme/theme.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/node.hpp>
#include <ftxui/screen/box.hpp>
#include <ftxui/screen/screen.hpp>

namespace ftxtui {

using ftxui::Element;
using ftxui::Elements;
using ftxui::Color;
using ftxui::BorderStyle;

namespace {

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

}  // namespace

Element build_markdown(std::string_view text, int width) {
    (void)width;
    if (text.empty()) return ftxui::text("");

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
            blocks.push_back(code_elem | ftxui::bgcolor(theme::T::Panel));
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

        // 表格：MVP 降级为普通行内文本（保留 | 分隔，可读）
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

Element build_context_gauge(int used, int limit) {
    if (limit <= 0) return ftxui::text("n/a");
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
            ftxui::text("✖ 错误") | ftxui::color(Color::RedLight) | ftxui::bold,
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
                ftxui::text(" 思考中…") | ftxui::color(c),
            });
        } else {
            std::string sec = std::format("{:.1f}s", msg.reasoning_ms / 1000.0);
            // Nerd Font： 灯泡=思考（默认白色），/ chevron=折叠指示
            std::string chevron = msg.reasoning_expanded ? "\uF078" : "\uF054";
            thinking_header = ftxui::hbox({
                ftxui::text("\uF0EB 思考 "),
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
            // Nerd Font： 扳手=工具（默认白色），/ chevron=折叠指示
            ftxui::text("\uF0AD "),
            ftxui::color(c)(ftxui::text(status)),
            t.done ? ftxui::text("  ") : ftxui::text(""),
            t.done ? ftxui::text(t.expanded ? "\uF078" : "\uF054")
                   : ftxui::text(""),
        }));
        if (t.done && t.expanded) {
            card_rows.push_back(ftxui::hbox({
                ftxui::text("    "),
                ftxui::flex(build_markdown(t.result, width))
            }));
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