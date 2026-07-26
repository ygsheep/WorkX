/**
 * @file command_panel.cpp
 * @brief 命令面板实现
 * @version 1.0.0
 */

#include "tui/widgets/command_panel.h"
#include "tui/core/terminal.h"
#include "tui/core/color_scheme.h"
#include "tui/core/platform/i_platform.h"

#include <cstdio>
#include <algorithm>

namespace tui {

CommandPanel::CommandPanel(Terminal* terminal)
    : m_terminal(terminal)
{
}

void CommandPanel::set_commands(const std::vector<CommandEntry>& commands) {
    m_all_commands = commands;
    rebuild_filtered();
}

void CommandPanel::set_filter(const std::string& prefix) {
    // prefix 来自输入行，如 "/he" → filter = "he"
    if (prefix.size() > 0 && prefix[0] == '/') {
        m_filter = prefix.substr(1);
    } else {
        m_filter = prefix;
    }
    rebuild_filtered();
}

void CommandPanel::move_up() {
    if (m_filtered.empty()) return;
    if (m_selected > 0) {
        m_selected--;
    } else {
        m_selected = static_cast<int>(m_filtered.size()) - 1;
    }
}

void CommandPanel::move_down() {
    if (m_filtered.empty()) return;
    if (m_selected < static_cast<int>(m_filtered.size()) - 1) {
        m_selected++;
    } else {
        m_selected = 0;
    }
}

std::string CommandPanel::get_completion() const {
    if (m_filtered.empty()) return "";

    const auto& cmd = m_all_commands[m_filtered[m_selected]];
    return "/" + cmd.name + " ";
}

const CommandEntry* CommandPanel::get_selected() const {
    if (m_filtered.empty()) return nullptr;
    return &m_all_commands[m_filtered[m_selected]];
}

void CommandPanel::set_visible(bool visible) {
    m_visible = visible;
    if (!visible) {
        m_last_rendered.clear();
    }
}

void CommandPanel::rebuild_filtered() {
    m_filtered.clear();
    m_selected = 0;

    for (size_t i = 0; i < m_all_commands.size(); i++) {
        const auto& cmd = m_all_commands[i];
        if (m_filter.empty() ||
            cmd.name.compare(0, m_filter.size(), m_filter) == 0) {
            m_filtered.push_back(i);
        }
    }

    m_active = !m_filtered.empty();
}

void CommandPanel::render() {
    if (!m_visible || m_filtered.empty()) {
        clear();
        return;
    }

    static constexpr int MAX_DISPLAY = 7;

    int height = m_terminal->get_terminal_height();
    int total = static_cast<int>(m_filtered.size());
    int display_count = std::min(MAX_DISPLAY, total);

    // 可滚动窗口：将选中项保持在可视区域中间
    int scroll_offset = m_selected - display_count / 2;
    if (scroll_offset < 0) scroll_offset = 0;
    if (scroll_offset + display_count > total) scroll_offset = total - display_count;

    // 先清除所有 MAX_DISPLAY 行，避免过滤后旧行残留
    int clear_start = height - 1 - MAX_DISPLAY;
    if (clear_start < 1) clear_start = 1;

    int render_start = height - 1 - display_count;
    if (render_start < 1) render_start = 1;

    std::string content;
    content += "\x1b[?25l";

    for (int i = 0; i < MAX_DISPLAY; i++) {
        int row = clear_start + i;
        if (row < 1) continue;
        char pos_cmd[32];
        snprintf(pos_cmd, sizeof(pos_cmd), "\x1b[%d;1H", row);
        content += pos_cmd;
        content += "\x1b[2K";
    }

    for (int i = 0; i < display_count; i++) {
        int idx = scroll_offset + i;
        auto actual_idx = m_filtered[idx];
        const auto& cmd = m_all_commands[actual_idx];

        int row = render_start + i;
        if (row < 1) continue;

        char pos_cmd[32];
        snprintf(pos_cmd, sizeof(pos_cmd), "\x1b[%d;1H", row);
        content += pos_cmd;

        bool is_selected = (idx == m_selected);
        if (is_selected) {
            content += std::string(get_color_ansi(ColorRole::CommandPanelHighlight));
        }

        content += " /";
        content += cmd.name;

        // F.6：padding 动态计算，避免小屏幕溢出
        // 原 `16 - name.size() - 1` 在 name 较长时为负数（已被 if 判定跳过），
        // 但 16 这个基准值对小终端过大；改为基于终端宽度的动态值，范围 [4, 16]
        int term_w = m_terminal->get_terminal_width();
        int pad_base = std::min(16, std::max(4, term_w / 8));
        int padding = pad_base - static_cast<int>(cmd.name.size()) - 1;
        if (padding > 0) content += std::string(padding, ' ');

        if (!is_selected) {
            content += std::string(get_color_ansi(ColorRole::Default));
        }

        content += std::string(get_color_ansi(ColorRole::CommandPanelDesc));
        content += " ";
        content += cmd.description;
        content += std::string(get_color_ansi(ColorRole::Default));


    }

    content += "\x1b[?25h";

    if (content == m_last_rendered) return;
    m_last_rendered = content;

    m_terminal->write_safe(content);
}

void CommandPanel::clear() {
    if (m_last_rendered.empty()) return;
    m_last_rendered.clear();
}

} // namespace tui
