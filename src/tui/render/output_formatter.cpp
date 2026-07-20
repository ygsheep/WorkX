/**
 * @file output_formatter.cpp
 * @brief 输出格式化器实现
 * @version 2.1.0
 */

#include "tui/render/output_formatter.h"
#include "tui/core/terminal.h"
#include "tui/core/color_scheme.h"
#include "tui/utils/utf8_utils.h"

namespace agent {

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
                render_text_line(m_text_line);
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
                // table buffering: line starts with | or already collecting a table
                bool line_starts_pipe = (pos < text.size() && text[pos] == '|');
                m_buffering_table_line = (line_starts_pipe || m_in_table);

                // 缩进输出移到 render_text_line 中（只对普通文本行输出，
                // heading / list / hr 不输出缩进，与原行为一致）
                // 注意：heading / horizontal rule / list item 的检测统一放到
                // \n 处理阶段（render_text_line），避免流式 chunk 边界把行首
                // 标记字符（`#`、`-`、`*`、`+`、数字）单独到达时误判为普通文本。
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
                // 跳过 lang 行的 \n：直接推进到下一行起始，不触发 push_back
                // （否则 lang 行的 \n 会 push_back 一个空 buf，导致代码块第一行是空行）
                pos = (nl != std::string_view::npos) ? nl + 1 : end;
                m_at_line_start = true;
                continue;
            } else {
                // buffer code line — 累积到 m_code_line_buf，遇 \n 才 push_back
                // 流式 feed 可能将一行代码拆成多个片段到达，必须拼接否则会误判为多行
                size_t end = (nl != std::string_view::npos) ? nl : text.size();
                m_code_line_buf.append(text.substr(pos, end - pos));
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
                // 代码行累积完毕，push_back 到 m_code_lines
                m_code_lines.push_back(std::move(m_code_line_buf));
                m_code_line_buf.clear();
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
                bool emitted_newline = false;
                if (!m_text_line.empty()) {
                    // 对完整文本行做 markdown 检测（heading / hr / list / inline）。
                    // 必须在 \n 处理时做，避免流式 chunk 边界把行首标记字符单独
                    // 到达时误判（例如 `-` 单独到达，is_list_item 返回 false）。
                    emitted_newline = render_text_line(m_text_line);
                    m_text_line.clear();
                }
                if (!emitted_newline) {
                    m_terminal->write("\n");
                }
            }
            m_at_line_start = true;
            pos = nl + 1;
        }
    }
}

void OutputFormatter::flush() {
    // flush pending text line（走 markdown 检测，与 \n 处理路径一致）
    if (!m_text_line.empty()) {
        render_text_line(m_text_line);
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
    m_code_line_buf.clear();
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
    // 末尾残留的代码行（最后一行无 \n 结尾时）需 flush 到 m_code_lines
    if (!m_code_line_buf.empty()) {
        m_code_lines.push_back(std::move(m_code_line_buf));
        m_code_line_buf.clear();
    }
    if (!m_code_lines.empty() || !m_code_lang.empty()) {
        int w = m_terminal->get_terminal_width();
        m_terminal->write(render_code_block(m_code_lang, m_code_lines, w));
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
    // E.4：嵌套在引用块/列表中的表格按缩进后的宽度计算列宽
    const int indent = m_indent_level * 2;
    const std::string rendered = render_table(table, w, indent);
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

bool OutputFormatter::render_text_line(const std::string& line) {
    if (line.empty()) return false;

    // 1. heading: "# xxx" ~ "###### xxx"
    if (line[0] == '#') {
        int level = 0;
        while (level < static_cast<int>(line.size()) && line[level] == '#') ++level;
        if (level >= 1 && level <= 6
            && level < static_cast<int>(line.size())
            && (line[level] == ' ' || line[level] == '\t')) {
            std::string rest = line.substr(level);
            size_t rb = 0;
            while (rb < rest.size() && (rest[rb] == ' ' || rest[rb] == '\t')) ++rb;
            m_terminal->write(render_heading(level, rest.substr(rb)));
            return true;  // render_heading 已含末尾 \n
        }
    }

    // 2. horizontal rule: "---" / "***" / "___"
    if (is_horizontal_rule(line)) {
        m_terminal->write(render_hr());
        return true;  // render_hr 已含末尾 \n
    }

    // 3. list item: "- " / "* " / "+ " / "N. "（支持行首缩进实现嵌套）
    if (is_list_item(line)) {
        m_terminal->write(render_list_item(line));
        return true;  // render_list_item 已含末尾 \n
    }

    // 4. 普通文本行：先输出缩进（若有），再 render_inline
    if (m_indent_level > 0) {
        std::string indent(m_indent_level * 2, ' ');
        m_terminal->set_color(ColorRole::Assistant);
        m_terminal->write(indent);
        m_terminal->reset_color();
    }
    m_terminal->set_color(ColorRole::Assistant);
    m_terminal->write(render_inline(line));
    m_terminal->reset_color();
    return false;  // 未输出 \n，调用方需自行 write("\n")
}

} // namespace agent
