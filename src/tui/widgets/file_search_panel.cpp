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
#include "tui/utils/utf8_utils.h"
#include "core/utils/file_index.h"

#include <cstdio>
#include <algorithm>

namespace tui {

using namespace agent;  // P0: tui→agent 类型引用过渡方案，后续 P2/P3 收紧到显式前缀

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

    static constexpr int MAX_DISPLAY = 10;

    int height = m_terminal->get_terminal_height();
    int total = static_cast<int>(m_results.size());
    // 显示行数上限：MAX_DISPLAY + 屏幕可用行（面板区域 [1, h-3]，输入行在 h-1）
    int display_count = std::min(MAX_DISPLAY, total);
    int max_rows = height - 4;  // 行 1..h-3 可用（h-4 保底 1 行，h>=5）
    if (display_count > max_rows) display_count = max_rows;
    if (display_count < 1) display_count = 1;

    // 可滚动窗口：将选中项保持在可视区域中间
    int scroll_offset = m_selected - display_count / 2;
    if (scroll_offset < 0) scroll_offset = 0;
    if (scroll_offset + display_count > total) scroll_offset = total - display_count;

    // 清除所有 MAX_DISPLAY 行。
    // 面板底行固定 h-3（overlay 保护区 [h-12, h-3] 内），不得越界到 h-2/h-1，
    // 否则关闭面板后该行无法由 overlay 快照恢复
    int clear_start = height - 2 - MAX_DISPLAY;
    if (clear_start < 1) clear_start = 1;

    int render_start = height - 2 - display_count;
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

        // 路径按终端剩余宽度截断，防止超宽物理换行导致面板行错位/残留
        // 行头固定 3 字符（" > " / "   "），行宽必须 ≤ term_w - 1
        int header_w = 3;
        int max_path_width = m_terminal->get_terminal_width() - header_w - 1;
        if (max_path_width < 4) max_path_width = 4;
        std::string path = entry.relative_path;
        if (entry.is_directory) {
            path += "/";
        }
        if (display_width(path) > max_path_width) {
            content += truncate_to_width(path, max_path_width - 1);
            content += "…";
        } else {
            content += path;
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

} // namespace tui
