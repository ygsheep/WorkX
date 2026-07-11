/**
 * @file output_formatter.cpp
 * @brief 输出格式化器实现
 * @version 2.1.0
 */

#include "tui/render/output_formatter.h"
#include "tui/core/terminal.h"
#include "tui/core/color_scheme.h"
#include "tui/utils/utf8_utils.h"

namespace workx {

// ---- 工具图标映射 ----

ToolIcon get_tool_icon(const std::string& tool_name) {
    if (tool_name.find("read") != std::string::npos ||
        tool_name.find("Read") != std::string::npos) {
        return {"\xe2\x97\x83", "<"};
    }
    if (tool_name.find("bash") != std::string::npos ||
        tool_name.find("shell") != std::string::npos ||
        tool_name.find("execute") != std::string::npos ||
        tool_name.find("Execute") != std::string::npos) {
        return {"\xe2\x96\xb8", ">"};
    }
    if (tool_name.find("grep") != std::string::npos ||
        tool_name.find("search") != std::string::npos ||
        tool_name.find("Search") != std::string::npos ||
        tool_name.find("find") != std::string::npos) {
        return {"\xe2\x97\x87", "*"};
    }
    if (tool_name.find("write") != std::string::npos ||
        tool_name.find("Write") != std::string::npos ||
        tool_name.find("edit") != std::string::npos ||
        tool_name.find("Edit") != std::string::npos) {
        return {"\xe2\x9c\x8e", "E"};
    }
    if (tool_name.find("agent") != std::string::npos ||
        tool_name.find("Agent") != std::string::npos) {
        return {"\xe2\x97\x8f", "@"};
    }
    return {"\xe2\x97\x83", "-"};
}

// ---- helpers ----

static std::string trim_start(std::string s) {
    while (!s.empty() && (s[0] == ' ' || s[0] == '\t'))
        s.erase(0, 1);
    return s;
}

// ---- OutputFormatter ----

OutputFormatter::OutputFormatter(Terminal* terminal)
    : m_terminal(terminal)
{
}

void OutputFormatter::feed(std::string_view text) {
    if (text.empty()) return;

    size_t pos = 0;
    while (pos < text.size()) {
        // ---- line-start handling ----
        if (m_at_line_start) {
            // flush any pending text line from previous chunk
            if (!m_text_line.empty()) {
                m_terminal->write(render_inline(m_text_line));
                m_text_line.clear();
            }

            // detect ```
            if (text.size() - pos >= 3 && text[pos] == '`' && text[pos+1] == '`' && text[pos+2] == '`') {
                if (!m_in_code_block) {
                    if (m_in_table) {
                        end_table();
                        m_table_buf.clear();
                        m_in_table = false;
                    }
                    m_buffering_table_line = false;
                    m_in_code_block = true;
                    m_code_lang.clear();
                    pos += 3;
                    continue;
                } else {
                    end_code_block();
                    pos += 3;
                    continue;
                }
            }

            if (!m_in_code_block) {
                // detect heading
                if (text[pos] == '#') {
                    size_t nl = text.find('\n', pos);
                    size_t end = (nl != std::string_view::npos) ? nl : text.size();
                    std::string line(text.substr(pos, end - pos));
                    int level = 0;
                    while (level < static_cast<int>(line.size()) && line[level] == '#')
                        level++;
                    if (level >= 1 && level <= 6) {
                        m_terminal->write(render_heading(level, trim_start(line.substr(level))));
                        pos = (nl != std::string_view::npos) ? nl + 1 : end;
                        m_at_line_start = (nl != std::string_view::npos);
                        continue;
                    }
                }

                // detect horizontal rule
                {
                    size_t nl = text.find('\n', pos);
                    size_t line_end = (nl != std::string_view::npos) ? nl : text.size();
                    if (is_horizontal_rule(text.substr(pos, line_end - pos))) {
                        m_terminal->write(render_hr());
                        pos = (nl != std::string_view::npos) ? nl + 1 : text.size();
                        m_at_line_start = (nl != std::string_view::npos);
                        continue;
                    }
                }

                // detect "- " / "* " bullet
                if (text.size() - pos >= 2 && (text[pos] == '-' || text[pos] == '*') && text[pos+1] == ' ') {
                    std::string indent(m_indent_level * 2, ' ');
                    m_terminal->set_color(ColorRole::Bullet);
                    m_terminal->write(indent + "\xe2\x80\xa2 ");
                    m_terminal->reset_color();
                    pos += 2;
                    m_at_line_start = false;
                    continue;
                }

                // table buffering: line starts with | or already collecting a table
                bool line_starts_pipe = (pos < text.size() && text[pos] == '|');
                m_buffering_table_line = (line_starts_pipe || m_in_table);

                if (!m_buffering_table_line && m_indent_level > 0) {
                    std::string indent(m_indent_level * 2, ' ');
                    m_terminal->set_color(ColorRole::Assistant);
                    m_terminal->write(indent);
                    m_terminal->reset_color();
                }
            }
            m_at_line_start = false;
        }

        // ---- find next newline ----
        auto nl = text.find('\n', pos);

        if (m_in_code_block) {
            if (m_code_lang.empty()) {
                // collecting language identifier (rest of the opening ``` line)
                size_t end = (nl != std::string_view::npos) ? nl : text.size();
                m_code_lang = trim_start(std::string(text.substr(pos, end - pos)));
                if (end == text.size() && nl == std::string_view::npos) {
                    return;  // no newline yet, lang continues next chunk
                }
                start_code_block();
                pos = end;
            } else {
                // buffer code line
                size_t end = (nl != std::string_view::npos) ? nl : text.size();
                m_code_lines.push_back(std::string(text.substr(pos, end - pos)));
                pos = end;
            }
        } else {
            // non-code path
            size_t end = (nl != std::string_view::npos) ? nl : text.size();
            if (m_buffering_table_line) {
                m_pending_line += std::string(text.substr(pos, end - pos));
            } else {
                if (end > pos) {
                    m_text_line += text.substr(pos, end - pos);
                }
            }
            pos = end;
        }

        // ---- handle newline ----
        if (nl != std::string_view::npos) {
            if (m_in_code_block) {
                // line already pushed above, nothing extra
            } else if (m_buffering_table_line) {
                if (m_table_buf.feed_line(m_pending_line)) {
                    m_in_table = m_table_buf.is_active();
                } else {
                    if (m_table_buf.is_complete()) {
                        end_table();
                    } else if (m_table_buf.is_invalid()) {
                        flush_table_as_text();
                    }
                    m_table_buf.clear();
                    m_in_table = false;
                    if (!m_pending_line.empty()) {
                        m_terminal->set_color(ColorRole::Assistant);
                        m_terminal->write(m_pending_line);
                        m_terminal->reset_color();
                    }
                    m_terminal->write("\n");
                }
                m_pending_line.clear();
                m_buffering_table_line = false;
            } else {
                if (!m_text_line.empty()) {
                    m_terminal->write(render_inline(m_text_line));
                    m_text_line.clear();
                }
                m_terminal->write("\n");
            }
            m_at_line_start = true;
            pos = nl + 1;
        }
    }
}

void OutputFormatter::flush() {
    // flush pending text line
    if (!m_text_line.empty()) {
        m_terminal->write(render_inline(m_text_line));
        m_text_line.clear();
    }
    if (m_in_code_block) {
        end_code_block();
    }
    if (m_in_table || m_buffering_table_line) {
        if (!m_pending_line.empty()) {
            if (m_table_buf.feed_line(m_pending_line)) {
                m_pending_line.clear();
            }
        }
        if (!m_table_buf.lines().empty()) {
            end_table();
        }
        if (!m_pending_line.empty()) {
            m_terminal->set_color(ColorRole::Assistant);
            m_terminal->write(m_pending_line);
            m_terminal->reset_color();
            m_pending_line.clear();
        }
        m_table_buf.clear();
        m_in_table = false;
        m_buffering_table_line = false;
    }
    m_at_line_start = true;
}

void OutputFormatter::reset() {
    m_in_code_block = false;
    m_code_lang.clear();
    m_at_line_start = true;
    m_code_lines.clear();
    m_text_line.clear();
    m_indent_level = 0;
    m_table_buf.clear();
    m_pending_line.clear();
    m_in_table = false;
    m_buffering_table_line = false;
}

void OutputFormatter::set_indent(int level) {
    m_indent_level = level < 0 ? 0 : level;
}

void OutputFormatter::start_code_block() {
    // lang is captured, lines will be collected; render happens in end_code_block
}

void OutputFormatter::end_code_block() {
    if (!m_code_lines.empty() || !m_code_lang.empty()) {
        m_terminal->write(render_code_block(m_code_lang, m_code_lines));
    }
    m_in_code_block = false;
    m_code_lang.clear();
    m_code_lines.clear();
}

void OutputFormatter::end_table() {
    const auto table = parse_table(m_table_buf.lines());
    if (!table.valid) {
        flush_table_as_text();
        return;
    }
    const int w = m_terminal->get_terminal_width();
    const std::string rendered = render_table(table, w);
    m_terminal->set_color(ColorRole::TextColor);
    m_terminal->write(rendered);
    m_terminal->reset_color();
}

void OutputFormatter::flush_table_as_text() {
    m_terminal->set_color(ColorRole::Assistant);
    for (const auto& line : m_table_buf.lines()) {
        m_terminal->write(line);
        m_terminal->write("\n");
    }
    m_terminal->reset_color();
}

} // namespace workx
