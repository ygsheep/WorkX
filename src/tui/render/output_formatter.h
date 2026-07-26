/**
 * @file output_formatter.h
 * @brief 输出格式化器
 * @details 结构化流式输出：代码块检测、bullet 替换、缩进管理
 * @version 2.0.0
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "tui/render/markdown_renderer.h"

namespace tui {

class Terminal;

struct ToolIcon {
    const char* icon;
    const char* fallback;
};

ToolIcon get_tool_icon(const std::string& tool_name);

class OutputFormatter {
public:
    explicit OutputFormatter(Terminal* terminal);

    void feed(std::string_view text);
    void flush();
    void reset();
    void set_indent(int level);

    int indent() const { return m_indent_level; }

private:
    void start_code_block();
    void end_code_block();

    void end_table();
    void flush_table_as_text();

    /// 对完整文本行做 markdown 检测（heading / hr / list / inline）并写入终端。
    /// 在 \n 处理时调用，避免流式 chunk 边界把行首 `-`/`#` 等标记字符单独到达时
    /// 误判为普通文本（line-start 阶段 is_list_item("-") 会返回 false）。
    /// @return true 表示已输出末尾换行符（heading/hr/list 内部含 \n）；
    ///         false 表示未输出（普通文本行），调用方需自行 write("\n")。
    bool render_text_line(const std::string& line);

    Terminal* m_terminal;

    bool m_in_code_block = false;
    std::string m_code_lang;
    bool m_at_line_start = true;
    int m_indent_level = 0;

    // collected code-block lines (rendered at close via render_code_block)
    std::vector<std::string> m_code_lines;
    // 当前代码行累积 buffer（流式 feed 跨片段拼接，遇 \n 才 push_back 到 m_code_lines）
    std::string m_code_line_buf;

    // normal text line buffer (processed at newline via render_inline / render_heading / render_hr)
    std::string m_text_line;

    // table buffering (minimal-invasive hybrid mode)
    TableBuffer m_table_buf;
    std::string m_pending_line;
    bool m_in_table = false;
    bool m_buffering_table_line = false;
};

} // namespace tui
