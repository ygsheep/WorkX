/**
 * @file composer.cpp
 * @brief 底部输入组件（自定义渲染：块状光标 + 灰色提示 + 快捷键处理）
 * @details 自绘单行输入，避免 ftxui::Input 的 Windows 光标回退问题
 *          （空输入时光标落在终端右下角）与默认 Bar 光标。
 *          Enter 提交；Shift+Tab 切换权限；Ctrl+O 思考视图。
 *          Ctrl+P 不在输入栏消费（全局搜索面板由 App 顶层捕获）。
 *          输入栏提示面板（"/" 命令 / "@" 文件）不占焦点：导航键在面板
 *          激活时经 suggest_* 回调转发给 App 状态机，否则走原逻辑。
 */

#include "widgets/composer.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include "theme/strings.h"
#include "theme/theme.h"

namespace ftxtui {

using ftxui::Color;
using ftxui::Component;
using ftxui::Element;
using ftxui::Event;

namespace {
/// @brief 输入区背景色（与侧边栏一致）
const Color kPanelBg = theme::T::Panel;

/// @brief 光标位置 pos 处字符的字节长度（UTF-8 安全；pos 越界返回 0）
size_t char_len_at(const std::string& s, size_t pos) {
    if (pos >= s.size()) return 0;
    size_t i = pos + 1;
    while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
    return i - pos;
}

/// @brief 返回 pos 前一个字符的字节起点（UTF-8 安全）
size_t prev_char(const std::string& s, size_t pos) {
    if (pos == 0 || pos > s.size()) return 0;
    size_t i = pos;
    while (i > 0) {
        --i;
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) return i;
    }
    return 0;
}

/// @brief 块状光标（白色实心单元）
Element cursor_block() {
    return ftxui::text(" ") | ftxui::bgcolor(Color::White);
}

/// @brief 把控制台/IME 光标锚定到给定元素所在单元（focused 时）。
///        focusCursorBar 会接管本组件的 focused 游标节点，FTXUI 据此把
///        真实控制台光标定位到该节点左上角。须只作用于"光标所在的那个字符
///        单元"，而不是整条输入元素——否则锚定到行首第 0 列，Windows IME
///        候选/拼音会显示在输入最前面而非输入点之后。
Element mark_cursor(Element e) {
    return ftxui::focusCursorBar(std::move(e));
}

/// @brief 渲染带块状光标的输入文本（focused 才显示光标）
///        空输入：光标块 + 灰色提示；非空：光标块覆盖当前字符
Element render_with_cursor(const std::string& text, size_t cursor, bool focused) {
    cursor = std::min(cursor, text.size());
    if (text.empty()) {
        Element ph = ftxui::text(std::string(str::kComposerPlaceholder)) |
                     ftxui::color(theme::T::Text);
        if (!focused) return ph;
        return ftxui::hbox({mark_cursor(cursor_block()), ph});
    }
    if (cursor >= text.size()) {
        Element e = ftxui::text(text) | ftxui::color(theme::T::Text);
        if (focused) {
            return ftxui::hbox({e, mark_cursor(cursor_block())});
        }
        return e;
    }
    size_t clen = char_len_at(text, cursor);
    Element at = ftxui::text(text.substr(cursor, clen));
    if (focused) {
        at = mark_cursor(at | ftxui::bgcolor(Color::White) | ftxui::color(Color::Black));
    } else {
        at |= ftxui::color(theme::T::Text);
    }
    return ftxui::hbox({
        ftxui::text(text.substr(0, cursor)) | ftxui::color(theme::T::Text),
        at,
        ftxui::text(text.substr(cursor + clen)) | ftxui::color(theme::T::Text),
    });
}
}  // namespace

Component make_composer(ComposerOptions& opt) {
    size_t internal_cursor = 0;  ///< 内部光标（opt.cursor 为空时使用）
    size_t& cursor = opt.cursor ? *opt.cursor : internal_cursor;

    // 粘贴脉冲检测：终端粘贴的多行容被拆成多个字符事件快速到达，其中的换行
    // 又会被解析成 Event::Return。靠"与上一次字符插入间隔极短且连续多次"
    // 判定当前处于粘贴中，从而把粘贴换行当作空格插入而非提交。
    // 状态必须堆分配并按值捕获：make_composer 返回后局部变量即失效，
    // 若捕获其引用，每次按键写已释放的栈内存会破坏调用栈（表现为
    // RunOnce 复制组件 shared_ptr 时访问违例）。
    struct PasteState {
        std::chrono::steady_clock::time_point last_insert{};
        int paste_burst = 0;
    };
    auto paste = std::make_shared<PasteState>();

    auto renderer = ftxui::Renderer([&opt, &cursor](bool focused) {
        Element body = render_with_cursor(*opt.buffer, cursor, focused)
                       | ftxui::bgcolor(kPanelBg);
        // 控制台/IME 光标的位置已由 render_with_cursor 中的 mark_cursor 锚定到
        // 插入点所在字符单元（而非整条输入元素行首），Windows IME 候选框才会
        // 跟随显示在真实插入位置。此处的 focusCursorBar 不再单独包裹。
        return body;
    });

    Component wrapped = renderer | ftxui::CatchEvent(
        [&opt, &cursor, paste, renderer](Event e) {
        try {
        std::string& text = *opt.buffer;
        cursor = std::min(cursor, text.size());

        // ---- 提示面板优先：激活时拦截导航键（面板不占焦点，事件经此处转发）----
        const bool suggest = opt.suggest_active && opt.suggest_active();

        if (e == Event::Return) {
            // 粘贴多行里的换行（\r\n → \n → Return）：不提交，转为一个空格。
            // 判定条件：刚从连续快速字符插入（粘贴脉冲）中收到 Return。
            const bool is_paste_newline =
                paste->paste_burst >= 2 && paste->last_insert != std::chrono::steady_clock::time_point{} &&
                (std::chrono::steady_clock::now() - paste->last_insert) <=
                    std::chrono::milliseconds(150);
            if (is_paste_newline) {
                text.insert(cursor, " ");
                cursor += 1;
                if (opt.suggest_refresh) opt.suggest_refresh();
                return true;
            }
            if (suggest && opt.suggest_enter && opt.suggest_enter()) return true;
            std::string t = text;
            size_t b = t.find_first_not_of(" \t");
            size_t en = t.find_last_not_of(" \t");
            if (b == std::string::npos) return true;  // 全空白
            std::string trimmed = t.substr(b, en - b + 1);
            if (opt.on_submit) opt.on_submit(trimmed);
            text.clear();
            cursor = 0;
            if (opt.suggest_refresh) opt.suggest_refresh();
            return true;
        }
        // Ctrl+Enter（Windows 补丁改写为 kitty 序列 \x1b[13;5u）：
        // 面板激活时插入引用（@路径），否则不消费
        if (e == ftxui::Event::Special("\x1b[13;5u")) {
            if (suggest && opt.suggest_enter_insert && opt.suggest_enter_insert())
                return true;
            return false;
        }
        if (e == Event::Tab) {
            if (suggest && opt.suggest_move) {
                opt.suggest_move(+1);  // Tab = 向下循环选择
                return true;
            }
            return true;  // 无面板时 Tab 无操作（防插入控制字符）
        }
        if (e == Event::Escape) {
            if (suggest && opt.suggest_cancel) {
                opt.suggest_cancel();
                return true;
            }
            return false;
        }
        if (e == Event::ArrowUp) {
            if (suggest && opt.suggest_move) { opt.suggest_move(-1); return true; }
            return false;
        }
        if (e == Event::ArrowDown) {
            if (suggest && opt.suggest_move) { opt.suggest_move(+1); return true; }
            return false;
        }

        if (e == Event::Backspace) {
            if (cursor > 0) {
                size_t p = prev_char(text, cursor);
                text.erase(p, cursor - p);
                cursor = p;
            }
            if (opt.suggest_refresh) opt.suggest_refresh();
            return true;
        }
        if (e == Event::Delete) {
            if (cursor < text.size()) text.erase(cursor, char_len_at(text, cursor));
            if (opt.suggest_refresh) opt.suggest_refresh();
            return true;
        }
        if (e == Event::ArrowLeft) {
            cursor = prev_char(text, cursor);
            return true;
        }
        if (e == Event::ArrowRight) {
            size_t n = char_len_at(text, cursor);
            cursor = std::min(text.size(), cursor + std::max<size_t>(1, n));
            return true;
        }
        if (e == Event::Home) {
            cursor = 0;
            return true;
        }
        if (e == Event::End) {
            cursor = text.size();
            return true;
        }
        if (e == Event::TabReverse) {  // Shift+Tab：提示面板激活时向上循环，否则切换权限
            if (suggest && opt.suggest_move) {
                opt.suggest_move(-1);
                return true;
            }
            if (opt.on_perm_toggle) opt.on_perm_toggle();
            return true;
        }
        // Ctrl 组合键：终端/VT 模式下可能以 Character 或 Special 形式到达
        auto is_ctrl = [&e](uint8_t c) {
            const std::string s(1, static_cast<char>(c));
            return (e.is_character() && e.character() == s) ||
                   e == ftxui::Event::Special(s);
        };
        if (is_ctrl(0x0f)) {  // Ctrl+O 思考视图
            if (opt.on_toggle_thinking) opt.on_toggle_thinking();
            return true;
        }
        if (is_ctrl(0x01)) {  // Ctrl+A 行首
            cursor = 0;
            return true;
        }
        if (is_ctrl(0x05)) {  // Ctrl+E 行尾
            cursor = text.size();
            return true;
        }
        if (e.is_character()) {
            const std::string& ch = e.character();
            if (ch.size() == 1 && static_cast<uint8_t>(ch[0]) < 0x20) return false;
            // 维护粘贴脉冲计数：距上次插入 ≤100ms 视为连续（粘贴），否则重新计数
            const auto now = std::chrono::steady_clock::now();
            if (paste->last_insert != std::chrono::steady_clock::time_point{} &&
                now - paste->last_insert <= std::chrono::milliseconds(100)) {
                ++paste->paste_burst;
            } else {
                paste->paste_burst = 1;
            }
            paste->last_insert = now;
            text.insert(cursor, ch);
            cursor += ch.size();
            if (opt.suggest_refresh) opt.suggest_refresh();
            return true;
        }
        if (e.is_mouse()) {
            if (e.mouse().button == ftxui::Mouse::Left &&
                e.mouse().motion == ftxui::Mouse::Pressed) {
                renderer->TakeFocus();
                return true;
            }
        }
        return false;
        } catch (...) {
            // 单次按键处理即便抛出（含非 std::exception 类型），也不应令整个程序退出
            return true;
        }
    });

    return wrapped;
}

}  // namespace ftxtui