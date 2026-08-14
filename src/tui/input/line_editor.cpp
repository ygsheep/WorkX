/**
 * @file line_editor.cpp
 * @brief 行编辑器实现
 * @details 参考 llama.cpp readline_advanced，面向 EventBus 设计
 */

#include "tui/input/line_editor.h"
#include "tui/core/platform/i_platform.h"
#include "tui/utils/utf8_utils.h"
#include "liblogger/logger.h"
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
static constexpr char32_t KEY_RESIZE           = 0xE00B;  // 终端尺寸变更
static constexpr char32_t KEY_BACKTAB          = 0xE00C;  // Shift+Tab = ESC [ Z（与 vt_input_decoder 对齐）
// KEY_WAKE 由 i_platform.h 统一导出（跨线程唤醒，AskUser 等）

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

void LineEditor::set_perm_toggle_callback(PermToggleCallback cb) {
    m_perm_toggle_cb = std::move(cb);
}

void LineEditor::set_tab_completed_callback(TabCompletedCallback cb) {
    m_tab_completed_cb = std::move(cb);
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

void LineEditor::set_resize_callback(ResizeCallback cb) {
    m_resize_cb = std::move(cb);
}

void LineEditor::set_edit_lines_callback(EditLinesCallback cb) {
    m_edit_lines_cb = std::move(cb);
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

// ---- 多行支持 ----

size_t LineEditor::line_count() const {
    size_t n = 1;
    for (char c : m_line) {
        if (c == '\n') ++n;
    }
    return n;
}

size_t LineEditor::cur_line_idx() const {
    size_t byte = 0;
    size_t line = 0;
    for (size_t c = 0; c < m_char_pos && byte < m_line.size(); ++c) {
        size_t advance = 0;
        char32_t cp = decode_utf8(m_line, byte, advance);
        if (cp == '\n') ++line;
        byte += advance;
    }
    return line;
}

size_t LineEditor::line_start_char(size_t line_idx) const {
    size_t byte = 0;
    size_t line = 0;
    for (size_t c = 0; c < m_widths.size(); ++c) {
        if (line == line_idx) return c;
        size_t advance = 0;
        char32_t cp = decode_utf8(m_line, byte, advance);
        if (cp == '\n') ++line;
        byte += advance;
    }
    return m_widths.size();
}

size_t LineEditor::line_char_count(size_t line_idx) const {
    size_t start = line_start_char(line_idx);
    if (line_idx + 1 < line_count()) {
        // 非最后一行：行尾还有一个 \n 字符
        return line_start_char(line_idx + 1) - start - 1;
    }
    return m_widths.size() - start;
}

size_t LineEditor::char_to_byte(size_t char_pos) const {
    if (char_pos >= m_widths.size()) return m_line.size();
    size_t byte = 0;
    for (size_t c = 0; c < char_pos; ++c) {
        size_t advance = 0;
        decode_utf8(m_line, byte, advance);
        byte += advance;
    }
    return byte;
}

char32_t LineEditor::char_at(size_t char_pos) const {
    size_t byte = char_to_byte(char_pos);
    size_t advance = 0;
    return decode_utf8(m_line, byte, advance);
}

std::string LineEditor::line_text(size_t line_idx) const {
    size_t start_byte = 0;
    for (size_t i = 0; i < line_idx; ++i) {
        size_t pos = m_line.find('\n', start_byte);
        start_byte = (pos == std::string::npos) ? m_line.size() : pos + 1;
    }
    size_t end_byte = m_line.find('\n', start_byte);
    if (end_byte == std::string::npos) end_byte = m_line.size();
    return m_line.substr(start_byte, end_byte - start_byte);
}

int LineEditor::line_prefix_width(size_t line_idx, size_t char_in_line) const {
    size_t start = line_start_char(line_idx);
    int w = 0;
    for (size_t c = start; c < start + char_in_line && c < m_widths.size(); ++c) {
        w += (m_widths[c] > 0 ? m_widths[c] : 1);
    }
    return w;
}

int LineEditor::input_area_max_lines() const {
    int max_lines = m_platform->get_terminal_height() - 3;
    if (max_lines < 1) max_lines = 1;
    return max_lines;
}

void LineEditor::redraw_input() {
    int term_h = m_platform->get_terminal_height();
    size_t total = line_count();
    int max_lines = input_area_max_lines();

    // 显示窗口：始终包含光标所在行
    size_t cur = cur_line_idx();
    size_t win_start = 0;
    if (total > static_cast<size_t>(max_lines)) {
        if (cur + 1 > static_cast<size_t>(max_lines)) {
            win_start = cur + 1 - static_cast<size_t>(max_lines);
        }
    }
    size_t win_lines = std::min(total - win_start, static_cast<size_t>(max_lines));
    if (win_lines < 1) win_lines = 1;

    // 行数减少时（删除换行/删除整行等），窗口顶部下移，
    // 需清空上一帧窗口上方多出的行，否则残留旧文本
    if (m_last_win_lines > win_lines) {
        int top_old = term_h - static_cast<int>(m_last_win_lines);
        int top_new = term_h - static_cast<int>(win_lines);
        for (int r = top_old; r < top_new; ++r) {
            char goto_cmd[32];
            snprintf(goto_cmd, sizeof(goto_cmd), "\x1b[%d;1H", r);
            m_platform->write_output(goto_cmd);
            m_platform->write_output("\x1b[2K");
        }
    }
    m_last_win_lines = win_lines;

    if (m_edit_lines_cb) {
        m_edit_lines_cb(static_cast<int>(win_lines));
    }

    // 重绘输入区（每行清空后输出）
    for (size_t i = 0; i < win_lines; ++i) {
        char goto_cmd[32];
        snprintf(goto_cmd, sizeof(goto_cmd), "\x1b[%d;1H", term_h - static_cast<int>(win_lines) + static_cast<int>(i));
        m_platform->write_output(goto_cmd);
        m_platform->write_output("\x1b[2K");
        if (i == 0) {
            m_platform->write_output(m_is_continuation ? "\xe2\x94\x82 " : m_prompt);
        }
        m_platform->write_output(line_text(win_start + i));
    }

    // 定位光标到编辑位置
    char goto_cmd[32];
    snprintf(goto_cmd, sizeof(goto_cmd), "\x1b[%d;1H",
             term_h - static_cast<int>(win_lines) + static_cast<int>(cur - win_start));
    m_platform->write_output(goto_cmd);
    int col = line_prefix_width(cur, m_char_pos - line_start_char(cur));
    // 窗口首行渲染了前缀（prompt 或 continuation 符号），光标列偏移需计入其显示宽度
    if (cur == win_start) {
        col += display_width(m_is_continuation ? "\xe2\x94\x82 " : m_prompt);
    }
    if (col > 0) {
        m_platform->move_cursor(col);
    }
    m_platform->flush();
}

// ---- 编辑操作 ----

void LineEditor::move_cursor_to(size_t char_pos) {
    if (char_pos > m_widths.size()) char_pos = m_widths.size();
    m_char_pos = char_pos;
    m_byte_pos = char_to_byte(char_pos);
    redraw_input();
}

void LineEditor::delete_at_cursor() {
    if (m_char_pos >= m_widths.size()) return;

    size_t next_pos = next_utf8_char_pos(m_line, m_byte_pos);
    size_t char_len = next_pos - m_byte_pos;

    m_line.erase(m_byte_pos, char_len);
    m_widths.erase(m_widths.begin() + static_cast<ptrdiff_t>(m_char_pos));

    redraw_input();
}

void LineEditor::set_line_contents(const std::string& new_line, int cursor_byte_pos) {
    m_line = new_line;
    m_widths.clear();
    m_byte_pos = 0;
    m_char_pos = 0;

    size_t idx = 0;
    while (idx < m_line.size()) {
        size_t advance = 0;
        char32_t cp = decode_utf8(m_line, idx, advance);
        int w = (cp == '\n') ? 0 : std::max(estimate_width(cp), 1);
        m_widths.push_back(w);
        idx += advance;
    }

    if (cursor_byte_pos >= 0) {
        size_t target = static_cast<size_t>(cursor_byte_pos);
        size_t byte = 0;
        size_t cp = 0;
        while (cp < m_widths.size()) {
            size_t adv = 0;
            decode_utf8(m_line, byte, adv);
            if (byte + adv > target) break;
            byte += adv;
            cp++;
        }
        m_char_pos = cp;
        m_byte_pos = byte;
    }

    redraw_input();
}

void LineEditor::move_to_line_start() {
    move_cursor_to(line_start_char(cur_line_idx()));
}

void LineEditor::move_to_line_end() {
    size_t line = cur_line_idx();
    move_cursor_to(line_start_char(line) + line_char_count(line));
}

void LineEditor::move_word_left() {
    if (m_char_pos == 0) return;

    size_t pos = m_char_pos;
    // 跳过空格
    while (pos > 0 && is_space_codepoint(char_at(pos - 1))) --pos;
    // 跳过单词
    while (pos > 0 && !is_space_codepoint(char_at(pos - 1))) --pos;

    move_cursor_to(pos);
}

void LineEditor::move_word_right() {
    if (m_char_pos >= m_widths.size()) return;

    size_t pos = m_char_pos;
    // 跳过空格
    while (pos < m_widths.size() && is_space_codepoint(char_at(pos))) ++pos;
    // 跳过单词
    while (pos < m_widths.size() && !is_space_codepoint(char_at(pos))) ++pos;
    // 跳过尾部空格
    while (pos < m_widths.size() && is_space_codepoint(char_at(pos))) ++pos;

    move_cursor_to(pos);
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
    // 重置行数残留跟踪：本次会话从单行开始，避免沿用上次多行会话的清理范围
    m_last_win_lines = 1;

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
        m_prompt = prompt;
        m_is_continuation = is_continuation;

        // 定位光标到输入区并完整重绘（同时通知 Terminal 调整滚动区）
        // \x1b[row;1H 绝对定位可以到达滚动区域外的行
        redraw_input();

        // 通知 Terminal 光标已离开输出区
        if (m_cursor_left_output_cb) {
            m_cursor_left_output_cb();
        }

        while (true) {
            assert(m_char_pos <= m_byte_pos);
            assert(m_char_pos <= m_widths.size());

            char32_t input_char = m_platform->read_char();
            LOG_INFO("[LineEditor] read_char returned input_char=0x{:X}", static_cast<unsigned>(input_char));

            // Enter 提交（Shift/Ctrl+Enter 由平台层转换为 '\n'）
            if (input_char == '\r') {
                break;
            }

            // 插入换行（Shift/Ctrl+Enter 或粘贴内容）
            if (input_char == '\n') {
                if (line_count() >= static_cast<size_t>(input_area_max_lines())) {
                    // 输入区行数达到上限：转为空格，避免编辑区超出屏幕
                    input_char = ' ';
                }
                std::string new_char_str;
                append_utf8(input_char, new_char_str);
                int w = (input_char == '\n') ? 0 : std::max(estimate_width(input_char), 1);
                m_line.insert(m_byte_pos, new_char_str);
                m_widths.insert(m_widths.begin() + static_cast<ptrdiff_t>(m_char_pos), w);
                m_byte_pos += new_char_str.length();
                m_char_pos++;
                redraw_input();
                if (m_input_changed_cb) m_input_changed_cb(m_line);
                continue;
            }

            // Tab 补全（仅单行输入）
            if (m_completion_cb && input_char == '\t' && line_count() == 1) {
                // 命令面板 Tab 补全（/ 开头时优先）
                if (!m_line.empty() && m_line[0] == '/' && m_command_tab_cb) {
                    auto completion = m_command_tab_cb();
                    if (!completion.empty() && completion != m_line) {
                        set_line_contents(completion, static_cast<int>(completion.size()));
                        if (m_input_changed_cb) m_input_changed_cb(m_line);
                        if (m_tab_completed_cb) m_tab_completed_cb();  // 收起命令面板
                        continue;
                    }
                }
                // 文件搜索面板 Tab 补全（包含 @ 时触发）
                if (m_line.find('@') != std::string::npos && m_command_tab_cb) {
                    auto completion = m_command_tab_cb();
                    if (!completion.empty() && completion != m_line) {
                        set_line_contents(completion, static_cast<int>(completion.size()));
                        if (m_input_changed_cb) m_input_changed_cb(m_line);
                        if (m_tab_completed_cb) m_tab_completed_cb();  // 收起文件搜索面板
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
                return {std::string(), false, false, false, false, true};
            }

            // #45：Shift+Tab 切换权限模式（Default/Plan/Bypass 三态，由外部回调处理）
            if (input_char == KEY_BACKTAB) {
                if (m_perm_toggle_cb) {
                    m_perm_toggle_cb();
                }
                continue;
            }

            // 跨线程唤醒（AskUser 等）：立即返回空结果，主循环检查 pending 事件
            if (input_char == KEY_WAKE) {
                LOG_INFO("[LineEditor] KEY_WAKE branch hit, returning woken_by_ask=true");
                ReadResult r;
                r.woken_by_ask = true;
                return r;
            }

            // 终端 resize：通知 Terminal 刷新 scroll region / 重放 DisplayBuffer，
            // 然后重新定位输入行并重绘当前编辑内容，继续读取用户输入（不丢失已输入文本）
            if (input_char == KEY_RESIZE) {
                if (m_resize_cb) {
                    m_resize_cb();
                }
                redraw_input();
                // handle_resize() 内部 setup_scroll_region_locked() 会把光标标记为"在输出区"，
                // 但 redraw_input() 后光标实际位于输入行。若不重新标记光标离开输出区，
                // 后台流式线程 write() 会误判光标位置，把输出文本写到输入行区域。
                if (m_cursor_left_output_cb) {
                    m_cursor_left_output_cb();
                }
                continue;
            }

            if (input_char == KEY_ARROW_LEFT) {
                if (m_char_pos > 0) {
                    m_char_pos--;
                    m_byte_pos = prev_utf8_char_pos(m_line, m_byte_pos);
                    redraw_input();
                }
            } else if (input_char == KEY_ARROW_RIGHT) {
                if (m_char_pos < m_widths.size()) {
                    m_char_pos++;
                    m_byte_pos = next_utf8_char_pos(m_line, m_byte_pos);
                    redraw_input();
                }
            } else if (input_char == KEY_ARROW_UP) {
                if (line_count() > 1) {
                    // 多行编辑：↑ 移动到上一行（保持列）
                    size_t line = cur_line_idx();
                    if (line > 0) {
                        size_t col = m_char_pos - line_start_char(line);
                        size_t target = line - 1;
                        size_t target_count = line_char_count(target);
                        if (col > target_count) col = target_count;
                        move_cursor_to(line_start_char(target) + col);
                    }
                } else {
                    // 命令面板/文件搜索面板模式：↑ 转发给面板
                    if (!is_continuation && !m_line.empty() && m_command_nav_cb &&
                        (m_line[0] == '/' || m_line.find('@') != std::string::npos)) {
                        if (m_command_nav_cb(input_char)) continue;
                    }
                    if (!is_continuation) history_prev();
                }
            } else if (input_char == KEY_ARROW_DOWN) {
                if (line_count() > 1) {
                    // 多行编辑：↓ 移动到下一行（保持列）
                    size_t line = cur_line_idx();
                    if (line + 1 < line_count()) {
                        size_t col = m_char_pos - line_start_char(line);
                        size_t target = line + 1;
                        size_t target_count = line_char_count(target);
                        if (col > target_count) col = target_count;
                        move_cursor_to(line_start_char(target) + col);
                    }
                } else {
                    // 命令面板/文件搜索面板模式：↓ 转发给面板
                    if (!is_continuation && !m_line.empty() && m_command_nav_cb &&
                        (m_line[0] == '/' || m_line.find('@') != std::string::npos)) {
                        if (m_command_nav_cb(input_char)) continue;
                    }
                    if (!is_continuation) history_next();
                }
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
                // Backspace（行首删除时合并上一行）
                if (m_char_pos > 0) {
                    size_t prev_pos = prev_utf8_char_pos(m_line, m_byte_pos);
                    size_t char_len = m_byte_pos - prev_pos;

                    m_line.erase(prev_pos, char_len);
                    m_widths.erase(m_widths.begin() + static_cast<ptrdiff_t>(m_char_pos - 1));
                    m_char_pos--;
                    m_byte_pos = prev_pos;

                    redraw_input();
                }
            } else if (input_char == 0x1B) {
                // 独立 Esc 键（#23 P3）：等同打断，与 Ctrl+C 语义一致。
                // 注意：转义序列已被解码器转换为 KEY_*，能走到这里的一定是孤立 Esc。
                m_platform->write_output("ESC\n");
                return {std::string(), false, false, true, true};
            } else {
                // 插入字符
                std::string new_char_str;
                append_utf8(input_char, new_char_str);
                int w = std::max(estimate_width(input_char), 1);

                m_line.insert(m_byte_pos, new_char_str);
                m_widths.insert(m_widths.begin() + static_cast<ptrdiff_t>(m_char_pos), w);
                m_byte_pos += new_char_str.length();
                m_char_pos++;

                redraw_input();
            }

            // 通知输入变化（用于命令面板过滤更新）
            if (m_input_changed_cb) {
                m_input_changed_cb(m_line);
            }
        }

        // ---- 回车处理 ----

        // 反斜杠续行（仅单行文本且末尾为 \ 时）
        if (line_count() == 1 && !m_line.empty()) {
            size_t last_start = prev_utf8_char_pos(m_line, m_line.size());
            size_t char_len = m_line.size() - last_start;
            if (char_len == 1 && m_line[last_start] == '\\') {
                m_line.pop_back();
                accumulated += m_line + "\n";
                m_platform->write_output("\n");
                is_continuation = true;
                m_history_idx = SIZE_MAX;
                m_backup_line.clear();
                continue;  // 回到外层 while，读取下一行
            }
        }

        m_platform->write_output("\n");

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
