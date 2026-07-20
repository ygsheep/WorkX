/**
 * @file file_search_panel.cpp
 * @brief 文件搜索面板实现
 * @version 1.0.0
 * @date 2026-07
 */

#include "tui/widgets/file_search_panel.h"
#include "tui/core/terminal.h"
#include "tui/core/color_scheme.h"
#include "tui/core/platform/i_platform.h"
#include "app/ui/file_index.h"

#include <cstdio>
#include <algorithm>

namespace agent {

FileSearchPanel::FileSearchPanel(Terminal* terminal)
    : m_terminal(terminal)
{
}

void FileSearchPanel::set_query(const std::string& query) {
    m_query = query;
    m_selected = 0;
    // 按需刷新文件索引（方案 E）：
    // - 工具写入/删除文件后会标记 dirty，此时触发重建
    // - 即便没有 dirty，超过 2 秒也会重建一次，覆盖外部文件变更
    // - 2 秒防抖避免每次按键都全量扫描目录
    global_file_index().refresh_if_needed(2000);
    search_files();
}

void FileSearchPanel::search_files() {
    m_results.clear();
    m_active = false;

    auto& index = global_file_index();
    if (!index.is_ready()) {
        return;
    }

    // 空查询返回 15 个最近修改的文件
    m_results = index.search(m_query, m_query.empty() ? 15 : 50);
    m_active = !m_results.empty();
}

void FileSearchPanel::move_up() {
    if (m_results.empty()) return;
    if (m_selected > 0) {
        m_selected--;
    } else {
        m_selected = static_cast<int>(m_results.size()) - 1;
    }
}

void FileSearchPanel::move_down() {
    if (m_results.empty()) return;
    if (m_selected < static_cast<int>(m_results.size()) - 1) {
        m_selected++;
    } else {
        m_selected = 0;
    }
}

std::string FileSearchPanel::get_selected_path() const {
    if (m_results.empty()) return "";
    std::string path = m_results[m_selected].relative_path;
    // 目录带尾部 /
    if (m_results[m_selected].is_directory) {
        path += "/";
    }
    return path;
}

void FileSearchPanel::set_visible(bool visible) {
    m_visible = visible;
    if (!visible) {
        m_last_rendered.clear();
    }
}

void FileSearchPanel::render() {
    if (!m_visible || m_results.empty()) {
        clear();
        return;
    }

    static constexpr int MAX_DISPLAY = 7;

    int height = m_terminal->get_terminal_height();
    int total = static_cast<int>(m_results.size());
    int display_count = std::min(MAX_DISPLAY, total);

    // 可滚动窗口：将选中项保持在可视区域中间
    int scroll_offset = m_selected - display_count / 2;
    if (scroll_offset < 0) scroll_offset = 0;
    if (scroll_offset + display_count > total) scroll_offset = total - display_count;

    // 清除所有 MAX_DISPLAY 行
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
        const auto& entry = m_results[idx];

        int row = render_start + i;
        if (row < 1) continue;

        char pos_cmd[32];
        snprintf(pos_cmd, sizeof(pos_cmd), "\x1b[%d;1H", row);
        content += pos_cmd;

        bool is_selected = (idx == m_selected);
        if (is_selected) {
            content += std::string(get_color_ansi(ColorRole::CommandPanelHighlight));
            content += " > ";
        } else {
            content += "   ";
        }

        content += entry.relative_path;
        if (entry.is_directory) {
            content += "/";
        }

        if (is_selected) {
            content += std::string(get_color_ansi(ColorRole::Default));
        }
    }

    content += "\x1b[?25h";

    if (content == m_last_rendered) return;
    m_last_rendered = content;

    m_terminal->write_safe(content);
}

void FileSearchPanel::clear() {
    if (m_last_rendered.empty()) return;
    m_last_rendered.clear();
}

} // namespace agent
