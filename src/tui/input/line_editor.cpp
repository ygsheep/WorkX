/**
 * @file line_editor.cpp
 * @brief 行编辑器实现
 * @details 参考 llama.cpp readline_advanced，面向 EventBus 设计
 */

#include "tui/input/line_editor.h"
#include "tui/core/platform/i_platform.h"
#include "tui/utils/utf8_utils.h"
#include <cassert>
#include <cstdio>
#include <cwctype>
#include <numeric>

namespace tui {

// Win32 特殊键码（与 platform_win32.cpp 一致）
static constexpr char32_t KEY_ARROW_LEFT       = 0xE000;
static constexpr char32_t KEY_ARROW_RIGHT      = 0xE001;
static constexpr char32_t KEY_ARROW_UP         = 0xE002;
static constexpr char32_t KEY_ARROW_DOWN       = 0xE003;
static constexpr char32_t KEY_HOME             = 0xE004;
static constexpr char32_t KEY_END              = 0xE005;
static constexpr char32_t KEY_CTRL_ARROW_LEFT  = 0xE006;
static constexpr char32_t KEY_CTRL_ARROW_RIGHT = 0xE007;
static constexpr char32_t KEY_DELETE           = 0xE008;
static constexpr char32_t KEY_CTRL_C           = 0xE009;
static constexpr char32_t KEY_CTRL_O           = 0xE00A;

LineEditor::LineEditor(IPlatform* platform)
    : m_platform(platform)
{
}

void LineEditor::set_completion_callback(CompletionCallback cb) {
    m_completion_cb = std::move(cb);
}

void LineEditor::set_command_nav_callback(CommandNavCallback cb) {
    m_command_nav_cb = std::move(cb);
}

void LineEditor::set_command_tab_callback(CommandTabCallback cb) {
    m_command_tab_cb = std::move(cb);
}

void LineEditor::set_input_changed_callback(InputChangedCallback cb) {
    m_input_changed_cb = std::move(cb);
}

void LineEditor::set_cursor_left_output_callback(CursorLeftOutputCallback cb) {
    m_cursor_left_output_cb = std::move(cb);
}

void LineEditor::set_editing_changed_callback(EditingChangedCallback cb) {
    m_editing_changed_cb = std::move(cb);
}

void LineEditor::load_history(const std::vector<std::string>& entries) {
    for (const auto& entry : entries) {
        if (!entry.empty()) {
            m_history.push_back(entry);
        }
    }
}

char32_t LineEditor::decode_utf8(const std::string& input, size_t pos, size_t& advance) {
    unsigned char c = static_cast<unsigned char>(input[pos]);
    if ((c & 0x80u) == 0u) {
        advance = 1;
        return c;
    }
    if ((c & 0xE0u) == 0xC0u && pos + 1 < input.size()) {
        unsigned char c1 = static_cast<unsigned char>(input[pos + 1]);
        if ((c1 & 0xC0u) != 0x80u) { advance = 1; return 0xFFFD; }
        advance = 2;
        return ((c & 0x1Fu) << 6) | (c1 & 0x3Fu);
    }
    if ((c & 0xF0u) == 0xE0u && pos + 2 < input.size()) {
        unsigned char c1 = static_cast<unsigned char>(input[pos + 1]);
        unsigned char c2 = static_cast<unsigned char>(input[pos + 2]);
        if ((c1 & 0xC0u) != 0x80u || (c2 & 0xC0u) != 0x80u) { advance = 1; return 0xFFFD; }
        advance = 3;
        return ((c & 0x0Fu) << 12) | ((c1 & 0x3Fu) << 6) | (c2 & 0x3Fu);
    }
    if ((c & 0xF8u) == 0xF0u && pos + 3 < input.size()) {
        unsigned char c1 = static_cast<unsigned char>(input[pos + 1]);
        unsigned char c2 = static_cast<unsigned char>(input[pos + 2]);
        unsigned char c3 = static_cast<unsigned char>(input[pos + 3]);
        if ((c1 & 0xC0u) != 0x80u || (c2 & 0xC0u) != 0x80u || (c3 & 0xC0u) != 0x80u) {
            advance = 1;
            return 0xFFFD;
        }
        advance = 4;
        return ((c & 0x07u) << 18) | ((c1 & 0x3Fu) << 12) | ((c2 & 0x3Fu) << 6) | (c3 & 0x3Fu);
    }
    advance = 1;
    return 0xFFFD;
}

void LineEditor::append_utf8(char32_t ch, std::string& out) {
    if (ch <= 0x7F) {
        out.push_back(static_cast<unsigned char>(ch));
    } else if (ch <= 0x7FF) {
        out.push_back(static_cast<unsigned char>(0xC0 | ((ch >> 6) & 0x1F)));
        out.push_back(static_cast<unsigned char>(0x80 | (ch & 0x3F)));
    } else if (ch <= 0xFFFF) {
        out.push_back(static_cast<unsigned char>(0xE0 | ((ch >> 12) & 0x0F)));
        out.push_back(static_cast<unsigned char>(0x80 | ((ch >> 6) & 0x3F)));
        out.push_back(static_cast<unsigned char>(0x80 | (ch & 0x3F)));
    } else if (ch <= 0x10FFFF) {
        out.push_back(static_cast<unsigned char>(0xF0 | ((ch >> 18) & 0x07)));
        out.push_back(static_cast<unsigned char>(0x80 | ((ch >> 12) & 0x3F)));
        out.push_back(static_cast<unsigned char>(0x80 | ((ch >> 6) & 0x3F)));
        out.push_back(static_cast<unsigned char>(0x80 | (ch & 0x3F)));
    }
}

size_t LineEditor::prev_utf8_char_pos(const std::string& line, size_t pos) {
    if (pos == 0) return 0;
    pos--;
    while (pos > 0 && (line[pos] & 0xC0) == 0x80) {
        pos--;
    }
    return pos;
}

size_t LineEditor::next_utf8_char_pos(const std::string& line, size_t pos) {
    if (pos >= line.length()) return line.length();
    pos++;
    while (pos < line.length() && (line[pos] & 0xC0) == 0x80) {
        pos++;
    }
    return pos;
}

int LineEditor::estimate_width(char32_t codepoint) {
    // 调用统一的 utf8_utils 宽度判定，保证 LineEditor 与渲染层一致
    // 注：Tab 在 LineEditor 上下文中由 handle_input 单独处理，此处返回 0 不影响
    return char32_width(codepoint);
}

bool LineEditor::is_space_codepoint(char32_t cp) {
    return std::iswspace(static_cast<wint_t>(cp)) != 0;
}

void LineEditor::delete_at_cursor() {
    if (m_char_pos >= m_widths.size()) return;

    size_t next_pos = next_utf8_char_pos(m_line, m_byte_pos);
    int w = m_widths[m_char_pos];
    size_t char_len = next_pos - m_byte_pos;

    m_line.erase(m_byte_pos, char_len);
    m_widths.erase(m_widths.begin() + static_cast<ptrdiff_t>(m_char_pos));

    // 重绘尾部
    size_t p = m_byte_pos;
    int tail_width = 0;
    for (size_t i = m_char_pos; i < m_widths.size(); ++i) {
        size_t following = next_utf8_char_pos(m_line, p);
        m_platform->put_codepoint(m_line.c_str() + p, following - p, m_widths[i]);
        tail_width += m_widths[i];
        p = following;
    }

    // 清除残余
    for (int i = 0; i < w; ++i) {
        m_platform->write_output(" ");
    }

    m_platform->move_cursor(-(tail_width + w));
}

void LineEditor::set_line_contents(const std::string& new_line, int cursor_byte_pos) {
    move_to_line_start();

    // 清除当前行
    int total_width = std::accumulate(m_widths.begin(), m_widths.end(), 0,
        [](int acc, int w) { return acc + (w > 0 ? w : 1); });
    if (total_width > 0) {
        std::string spaces(static_cast<size_t>(total_width), ' ');
        m_platform->write_output(spaces);
        m_platform->move_cursor(-total_width);
    }

    m_line = new_line;
    m_widths.clear();
    m_byte_pos = 0;
    m_char_pos = 0;

    size_t idx = 0;
    int back_width = 0;
    while (idx < m_line.size()) {
        size_t advance = 0;
        char32_t cp = decode_utf8(m_line, idx, advance);
        int expected_width = estimate_width(cp);
        int real_width = m_platform->put_codepoint(m_line.c_str() + idx, advance, expected_width);
        if (real_width < 0) real_width = 0;
        m_widths.push_back(real_width);
        idx += advance;
        if (cursor_byte_pos >= 0 && static_cast<size_t>(cursor_byte_pos) < idx) {
            back_width += real_width;
        } else {
            ++m_char_pos;
            m_byte_pos = idx;
        }
    }
    if (cursor_byte_pos >= 0) {
        m_platform->move_cursor(-back_width);
    }
}

void LineEditor::move_to_line_start() {
    int back_width = 0;
    for (size_t i = 0; i < m_char_pos; ++i) {
        back_width += m_widths[i];
    }
    m_platform->move_cursor(-back_width);
    m_char_pos = 0;
    m_byte_pos = 0;
}

void LineEditor::move_to_line_end() {
    int forward_width = 0;
    for (size_t i = m_char_pos; i < m_widths.size(); ++i) {
        forward_width += m_widths[i];
    }
    m_platform->move_cursor(forward_width);
    m_char_pos = m_widths.size();
    m_byte_pos = m_line.length();
}

void LineEditor::move_word_left() {
    if (m_char_pos == 0) return;

    size_t new_char_pos = m_char_pos;
    size_t new_byte_pos = m_byte_pos;
    int move_width = 0;

    // 跳过空格
    while (new_char_pos > 0) {
        size_t prev_byte = prev_utf8_char_pos(m_line, new_byte_pos);
        size_t advance = 0;
        char32_t cp = decode_utf8(m_line, prev_byte, advance);
        if (!is_space_codepoint(cp)) break;
        move_width += m_widths[new_char_pos - 1];
        new_char_pos--;
        new_byte_pos = prev_byte;
    }

    // 跳过单词
    while (new_char_pos > 0) {
        size_t prev_byte = prev_utf8_char_pos(m_line, new_byte_pos);
        size_t advance = 0;
        char32_t cp = decode_utf8(m_line, prev_byte, advance);
        if (is_space_codepoint(cp)) break;
        move_width += m_widths[new_char_pos - 1];
        new_char_pos--;
        new_byte_pos = prev_byte;
    }

    m_platform->move_cursor(-move_width);
    m_char_pos = new_char_pos;
    m_byte_pos = new_byte_pos;
}

void LineEditor::move_word_right() {
    if (m_char_pos >= m_widths.size()) return;

    size_t new_char_pos = m_char_pos;
    size_t new_byte_pos = m_byte_pos;
    int move_width = 0;

    // 跳过空格
    while (new_char_pos < m_widths.size()) {
        size_t advance = 0;
        char32_t cp = decode_utf8(m_line, new_byte_pos, advance);
        if (!is_space_codepoint(cp)) break;
        move_width += m_widths[new_char_pos];
        new_char_pos++;
        new_byte_pos += advance;
    }

    // 跳过单词
    while (new_char_pos < m_widths.size()) {
        size_t advance = 0;
        char32_t cp = decode_utf8(m_line, new_byte_pos, advance);
        if (is_space_codepoint(cp)) break;
        move_width += m_widths[new_char_pos];
        new_char_pos++;
        new_byte_pos += advance;
    }

    // 跳过尾部空格
    while (new_char_pos < m_widths.size()) {
        size_t advance = 0;
        char32_t cp = decode_utf8(m_line, new_byte_pos, advance);
        if (!is_space_codepoint(cp)) break;
        move_width += m_widths[new_char_pos];
        new_char_pos++;
        new_byte_pos += advance;
    }

    m_platform->move_cursor(move_width);
    m_char_pos = new_char_pos;
    m_byte_pos = new_byte_pos;
}

void LineEditor::history_prev() {
    if (m_history.empty()) return;

    if (m_history_idx == SIZE_MAX) {
        m_backup_line = m_line;
        m_history_idx = m_history.size();
    }

    if (m_history_idx > 0) {
        m_history_idx--;
    }
    set_line_contents(m_history[m_history_idx]);
}

void LineEditor::history_next() {
    if (m_history.empty() || m_history_idx == SIZE_MAX) return;

    m_history_idx++;
    if (m_history_idx >= m_history.size()) {
        set_line_contents(m_backup_line);
        m_history_idx = SIZE_MAX;
        m_backup_line.clear();
    } else {
        set_line_contents(m_history[m_history_idx]);
    }
}

LineEditor::ReadResult LineEditor::read_line(const std::string& prompt) {
    // 重置历史浏览状态，避免上次浏览到历史中间项后新输入从中间项开始
    // m_history_idx = SIZE_MAX 表示未在浏览历史
    m_history_idx = SIZE_MAX;
    m_backup_line.clear();

    // 通知 Terminal：read_line() 开始运行
    if (m_editing_changed_cb) {
        m_editing_changed_cb(true);
    }

    // RAII guard：函数退出时通知 read_line() 结束
    struct EditingGuard {
        LineEditor* editor;
        ~EditingGuard() {
            if (editor->m_editing_changed_cb) {
                editor->m_editing_changed_cb(false);
            }
        }
    } guard{this};

    std::string accumulated;     // 续行累积内容
    bool is_continuation = false;

    while (true) {
        // 重置当前行编辑状态
        m_line.clear();
        m_widths.clear();
        m_char_pos = 0;
        m_byte_pos = 0;
        bool is_special_char = false;

        // 定位光标到输入行（倒数第 2 行）
        // \x1b[row;1H 绝对定位可以到达滚动区域外的行
        int term_h = m_platform->get_terminal_height();
        int input_row = term_h - 1;
        if (input_row < 1) input_row = 1;
        char goto_input[32];
        snprintf(goto_input, sizeof(goto_input), "\x1b[%d;1H", input_row);
        m_platform->write_output(goto_input);
        m_platform->write_output("\x1b[2K");  // 清除输入行残留
        m_platform->flush();

        // 通知 Terminal 光标已离开输出区
        if (m_cursor_left_output_cb) {
            m_cursor_left_output_cb();
        }

        // 显示提示符（续行时用 "│ " 代替主提示符）
        m_platform->write_output(is_continuation ? "\xe2\x94\x82 " : prompt);  // │
        m_platform->flush();

        while (true) {
            assert(m_char_pos <= m_byte_pos);
            assert(m_char_pos <= m_widths.size());

            m_platform->flush();
            char32_t input_char = m_platform->read_char();

            if (input_char == '\r' || input_char == '\n') {
                break;
            }

            // Tab 补全
            if (m_completion_cb && input_char == '\t') {
                // 命令面板 Tab 补全（/ 开头时优先）
                if (!m_line.empty() && m_line[0] == '/' && m_command_tab_cb) {
                    auto completion = m_command_tab_cb();
                    if (!completion.empty() && completion != m_line) {
                        set_line_contents(completion, static_cast<int>(completion.size()));
                        if (m_input_changed_cb) m_input_changed_cb(m_line);
                        continue;
                    }
                }
                // 文件搜索面板 Tab 补全（包含 @ 时触发）
                if (m_line.find('@') != std::string::npos && m_command_tab_cb) {
                    auto completion = m_command_tab_cb();
                    if (!completion.empty() && completion != m_line) {
                        set_line_contents(completion, static_cast<int>(completion.size()));
                        if (m_input_changed_cb) m_input_changed_cb(m_line);
                        continue;
                    }
                }
                // 通用 Tab 补全
                auto candidates = m_completion_cb(m_line, m_byte_pos);
                if (!candidates.empty()) {
                    const auto& best = candidates[0];
                    if (best.text != m_line) {
                        set_line_contents(best.text, static_cast<int>(best.cursor_pos));
                        if (m_input_changed_cb) m_input_changed_cb(m_line);
                    }
                    continue;
                }
            }

            if (input_char == WEOF || input_char == 0x04) {
                if (!accumulated.empty()) {
                    ReadResult r;
                    r.text = accumulated + m_line;
                    r.stream_end = true;
                    return r;
                }
                return {std::string(), true, false};
            }

            // Ctrl+C 中断
            if (input_char == KEY_CTRL_C) {
                m_platform->write_output("^C\n");
                return {std::string(), false, false, true};
            }

            // Ctrl+O 切换思考视图
            if (input_char == KEY_CTRL_O) {
                return {std::string(), false, false, false, true};
            }

            // 反斜杠/斜杠特殊标记（续行和命令）
            if (is_special_char) {
                // 替换显示：恢复原始字符
                m_platform->move_cursor(-1);
                m_platform->write_output(m_line.substr(m_line.size() - 1));
                is_special_char = false;
            }

            // 导航键（续行中禁用历史导航）
            if (input_char == KEY_ARROW_LEFT) {
                if (m_char_pos > 0) {
                    int w = m_widths[m_char_pos - 1];
                    m_platform->move_cursor(-w);
                    m_char_pos--;
                    m_byte_pos = prev_utf8_char_pos(m_line, m_byte_pos);
                }
            } else if (input_char == KEY_ARROW_RIGHT) {
                if (m_char_pos < m_widths.size()) {
                    int w = m_widths[m_char_pos];
                    m_platform->move_cursor(w);
                    m_char_pos++;
                    m_byte_pos = next_utf8_char_pos(m_line, m_byte_pos);
                }
            } else if (input_char == KEY_ARROW_UP) {
                // 命令面板/文件搜索面板模式：↑ 转发给面板
                if (!is_continuation && !m_line.empty() && m_command_nav_cb &&
                    (m_line[0] == '/' || m_line.find('@') != std::string::npos)) {
                    if (m_command_nav_cb(input_char)) continue;
                }
                if (!is_continuation) history_prev();
            } else if (input_char == KEY_ARROW_DOWN) {
                // 命令面板/文件搜索面板模式：↓ 转发给面板
                if (!is_continuation && !m_line.empty() && m_command_nav_cb &&
                    (m_line[0] == '/' || m_line.find('@') != std::string::npos)) {
                    if (m_command_nav_cb(input_char)) continue;
                }
                if (!is_continuation) history_next();
            } else if (input_char == KEY_HOME) {
                move_to_line_start();
            } else if (input_char == KEY_END) {
                move_to_line_end();
            } else if (input_char == KEY_CTRL_ARROW_LEFT) {
                move_word_left();
            } else if (input_char == KEY_CTRL_ARROW_RIGHT) {
                move_word_right();
            } else if (input_char == KEY_DELETE) {
                delete_at_cursor();
            } else if (input_char == 0x08 || input_char == 0x7F) {
                // Backspace
                if (m_char_pos > 0) {
                    int w = m_widths[m_char_pos - 1];
                    m_platform->move_cursor(-w);
                    m_char_pos--;
                    size_t prev_pos = prev_utf8_char_pos(m_line, m_byte_pos);
                    size_t char_len = m_byte_pos - prev_pos;
                    m_byte_pos = prev_pos;

                    m_line.erase(m_byte_pos, char_len);
                    m_widths.erase(m_widths.begin() + static_cast<ptrdiff_t>(m_char_pos));

                    // 重绘尾部
                    size_t p = m_byte_pos;
                    int tail_width = 0;
                    for (size_t i = m_char_pos; i < m_widths.size(); ++i) {
                        size_t next_p = next_utf8_char_pos(m_line, p);
                        m_platform->put_codepoint(m_line.c_str() + p, next_p - p, m_widths[i]);
                        tail_width += m_widths[i];
                        p = next_p;
                    }

                    // 清除残余
                    for (int i = 0; i < w; ++i) {
                        m_platform->write_output(" ");
                    }
                    m_platform->move_cursor(-(tail_width + w));
                }
            } else {
                // 插入字符
                std::string new_char_str;
                append_utf8(input_char, new_char_str);
                int w = estimate_width(input_char);

                if (m_char_pos == m_widths.size()) {
                    // 末尾插入
                    m_line += new_char_str;
                    int real_w = m_platform->put_codepoint(new_char_str.c_str(), new_char_str.length(), w);
                    if (real_w < 0) real_w = 0;
                    m_widths.push_back(real_w);
                    m_byte_pos += new_char_str.length();
                    m_char_pos++;
                } else {
                    // 中间插入
                    m_line.insert(m_byte_pos, new_char_str);
                    int real_w = m_platform->put_codepoint(new_char_str.c_str(), new_char_str.length(), w);
                    if (real_w < 0) real_w = 0;
                    m_widths.insert(m_widths.begin() + static_cast<ptrdiff_t>(m_char_pos), real_w);

                    // 重绘尾部
                    size_t p = m_byte_pos + new_char_str.length();
                    int tail_width = 0;
                    for (size_t i = m_char_pos + 1; i < m_widths.size(); ++i) {
                        size_t next_p = next_utf8_char_pos(m_line, p);
                        m_platform->put_codepoint(m_line.c_str() + p, next_p - p, m_widths[i]);
                        tail_width += m_widths[i];
                        p = next_p;
                    }
                    m_platform->move_cursor(-tail_width);

                    m_byte_pos += new_char_str.length();
                    m_char_pos++;
                }
            }

            // 反斜杠续行 / 斜杠命令 标记
            // E.8：使用 prev_utf8_char_pos 定位末尾完整字符的起始字节，
            // 确保 multi-byte UTF-8 字符不会被 m_line.back() 取到的末尾字节
            // 误判（虽然 UTF-8 续字节 0x80-0xBF 不包含 '\\' 或 '/'，但显式
            // 检查字符长度更稳健，也为未来扩展留余地）
            if (!m_line.empty()) {
                size_t last_start = prev_utf8_char_pos(m_line, m_line.size());
                size_t char_len = m_line.size() - last_start;
                // 仅对单字节 ASCII backslash/slash 触发特殊标记
                if (char_len == 1) {
                    char last_ch = m_line[last_start];
                    if (last_ch == '\\' || last_ch == '/') {
                        // 在末尾显示高亮替换
                        m_platform->move_cursor(-1);
                        m_platform->write_output("\x1b[7m");  // 反色
                        m_platform->write_output(m_line.substr(last_start, 1));
                        m_platform->write_output("\x1b[0m");  // 重置
                        is_special_char = true;
                    }
                }
            }

            // 通知输入变化（用于命令面板过滤更新）
            if (m_input_changed_cb) {
                m_input_changed_cb(m_line);
            }
        }

        // ---- 回车处理 ----

        if (is_special_char) {
            // 恢复正常显示
            m_platform->move_cursor(-1);
            m_platform->write_output(" ");
            m_platform->move_cursor(-1);

            char last = m_line.back();
            m_line.pop_back();

            if (last == '\\') {
                // 续行：累积当前行，进入续行循环
                accumulated += m_line + "\n";
                m_platform->write_output("\n");
                is_continuation = true;
                m_history_idx = SIZE_MAX;
                m_backup_line.clear();
                continue;  // 回到外层 while，读取下一行
            }
            // 斜杠命令：直接提交
            m_platform->write_output("\n");
        } else {
            m_platform->write_output("\n");
        }

        // 拼接累积内容 + 当前行
        ReadResult result;
        result.stream_end = false;
        result.text = accumulated + m_line;

        // 检测命令（以 / 开头，且非续行）
        result.is_command = !is_continuation && !result.text.empty() && result.text[0] == '/';

        // 添加到历史
        if (!result.text.empty()) {
            if (m_history.empty() || m_history.back() != result.text) {
                m_history.push_back(result.text);
            }
        }

        m_history_idx = SIZE_MAX;
        m_backup_line.clear();

        return result;
    }
}

} // namespace tui
