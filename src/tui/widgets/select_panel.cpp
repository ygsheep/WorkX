/**
 * @file select_panel.cpp
 * @brief 选择面板实现
 * @version 1.0.0
 */

#include "tui/widgets/select_panel.h"
#include "tui/core/terminal.h"
#include "tui/core/color_scheme.h"
#include "tui/core/platform/i_platform.h"

#include <algorithm>

namespace agent {

SelectPanel::SelectPanel(Terminal* terminal, Screen* screen)
    : m_terminal(terminal)
    , m_screen(screen)
{
}

void SelectPanel::set_tabs(const std::vector<SelectTab>& tabs) {
    m_tabs = tabs;
    m_active_tab = 0;
    m_cursor_row = 0;
}

void SelectPanel::next_tab() {
    if (m_tabs.size() <= 1) return;
    m_active_tab = (m_active_tab + 1) % static_cast<int>(m_tabs.size());
    m_cursor_row = 0;
}

void SelectPanel::prev_tab() {
    if (m_tabs.size() <= 1) return;
    m_active_tab = (m_active_tab - 1 + static_cast<int>(m_tabs.size()))
                   % static_cast<int>(m_tabs.size());
    m_cursor_row = 0;
}

void SelectPanel::move_up() {
    if (m_cursor_row > 0) m_cursor_row--;
}

void SelectPanel::move_down() {
    if (m_tabs.empty()) return;
    const auto& items = m_tabs[m_active_tab].items;
    if (m_cursor_row < static_cast<int>(items.size()) - 1) m_cursor_row++;
}

void SelectPanel::toggle_current() {
    if (m_tabs.empty()) return;
    auto& items = m_tabs[m_active_tab].items;
    if (m_cursor_row >= 0 && m_cursor_row < static_cast<int>(items.size())) {
        items[m_cursor_row].selected = !items[m_cursor_row].selected;
    }
}

std::vector<std::string> SelectPanel::get_selected_ids() const {
    std::vector<std::string> result;
    for (const auto& tab : m_tabs) {
        for (const auto& item : tab.items) {
            if (item.selected) {
                result.push_back(item.id);
            }
        }
    }
    return result;
}

const SelectItem* SelectPanel::get_current_item() const {
    if (m_tabs.empty()) return nullptr;
    const auto& items = m_tabs[m_active_tab].items;
    if (m_cursor_row >= 0 && m_cursor_row < static_cast<int>(items.size())) {
        return &items[m_cursor_row];
    }
    return nullptr;
}

void SelectPanel::activate_input_mode() {
    m_input_mode = true;
    m_input_buffer.clear();
}

void SelectPanel::deactivate_input_mode() {
    m_input_mode = false;
}

bool SelectPanel::is_input_mode() const {
    return m_input_mode;
}

const std::string& SelectPanel::get_input_text() const {
    return m_input_buffer;
}

void SelectPanel::input_char(char32_t ch) {
    if (ch <= 0x7F) {
        m_input_buffer.push_back(static_cast<unsigned char>(ch));
    } else if (ch <= 0x7FF) {
        m_input_buffer.push_back(static_cast<unsigned char>(0xC0 | ((ch >> 6) & 0x1F)));
        m_input_buffer.push_back(static_cast<unsigned char>(0x80 | (ch & 0x3F)));
    } else if (ch <= 0xFFFF) {
        m_input_buffer.push_back(static_cast<unsigned char>(0xE0 | ((ch >> 12) & 0x0F)));
        m_input_buffer.push_back(static_cast<unsigned char>(0x80 | ((ch >> 6) & 0x3F)));
        m_input_buffer.push_back(static_cast<unsigned char>(0x80 | (ch & 0x3F)));
    } else if (ch <= 0x10FFFF) {
        m_input_buffer.push_back(static_cast<unsigned char>(0xF0 | ((ch >> 18) & 0x07)));
        m_input_buffer.push_back(static_cast<unsigned char>(0x80 | ((ch >> 12) & 0x3F)));
        m_input_buffer.push_back(static_cast<unsigned char>(0x80 | ((ch >> 6) & 0x3F)));
        m_input_buffer.push_back(static_cast<unsigned char>(0x80 | (ch & 0x3F)));
    }
}

void SelectPanel::input_backspace() {
    if (m_input_buffer.empty()) return;
    size_t pos = m_input_buffer.size() - 1;
    while (pos > 0 && (static_cast<unsigned char>(m_input_buffer[pos]) & 0xC0) == 0x80) {
        pos--;
    }
    m_input_buffer.erase(pos);
}

void SelectPanel::set_title(const std::string& title) {
    m_title = title;
}

int SelectPanel::panel_height() const {
    if (m_tabs.empty()) return 0;
    int items_h = static_cast<int>(m_tabs[m_active_tab].items.size());
    // 1 (边框/标题) + 1 (Tab栏) + items + 1 (提示行)
    int h = 2 + items_h + 1;
    if (m_input_mode) h += 1;  // 输入框行
    return h;
}

void SelectPanel::render() {
    if (m_tabs.empty()) return;

    m_screen->clear();

    int term_h = m_terminal->get_terminal_height();
    int term_w = m_screen->width();

    // 面板从底部向上展开（留 3 行给 StatusBar + 输入行 + CommandPanel）
    // F.7：小屏幕上 panel_h 可能超过 term_h - 3，需要裁剪 panel_h 并从顶部开始
    int panel_h = panel_height();
    int start_row;
    if (panel_h + 3 > term_h) {
        // 面板超出屏幕，从顶部开始，裁剪 panel_h 到可用高度
        start_row = 0;
        panel_h = std::max(1, term_h - 3);
    } else {
        start_row = term_h - panel_h - 3;
    }

    int row = start_row;

    // 标题行
    if (!m_title.empty()) {
        m_screen->write(row, 0, " " + m_title, ColorRole::StatusBar);
        // 填充剩余宽度
        int fill_start = 1 + static_cast<int>(m_title.size());
        for (int c = fill_start; c < term_w; c++) {
            m_screen->write(row, c, "\xe2\x94\x80", ColorRole::StatusBar);  // ─
        }
        row++;
    }

    // Tab 栏: [Tab1] [Tab2] ...
    int col = 1;
    for (int i = 0; i < static_cast<int>(m_tabs.size()); i++) {
        ColorRole color = (i == m_active_tab) ? ColorRole::SelectTabActive
                                              : ColorRole::SelectTab;
        std::string tab_text = "[" + m_tabs[i].name + "]";
        m_screen->write(row, col, tab_text, color);
        col += static_cast<int>(tab_text.size()) + 1;
    }
    row++;

    // 选项列表
    const auto& items = m_tabs[m_active_tab].items;
    for (int i = 0; i < static_cast<int>(items.size()); i++) {
        // 图标: ◉(当前行/绿色) 或 ○(其他/灰色)
        std::string icon = (i == m_cursor_row) ? "\xe2\x97\x89 " : "\xe2\x97\x8b ";  // ◉ / ○
        ColorRole icon_color = (i == m_cursor_row) ? ColorRole::SelectChecked
                                                    : ColorRole::SelectUnchecked;

        m_screen->write(row, 1, icon, icon_color);
        m_screen->write(row, 3, items[i].display,
                        (i == m_cursor_row) ? ColorRole::UserInput : ColorRole::Default);
        row++;
    }

    // 内联输入框（输入模式时显示）
    if (m_input_mode) {
        m_screen->write(row, 1, "> ", ColorRole::Prompt);
        m_screen->write(row, 5, m_input_buffer, ColorRole::UserInput);
        m_screen->write(row, 5 + static_cast<int>(m_input_buffer.size()),
                        "\xe2\x96\x88", ColorRole::SelectCursor);  // █ 光标块
        row++;
    }

    // 提示行
    if (m_input_mode) {
        m_screen->write(row, 1,
            "\xe8\xbe\x93\xe5\x85\xa5\xe6\xa8\xa1\xe5\x9e\x8b ID  \xe5\x9b\x9e\xe8\xbd\xa6 \xe7\xa1\xae\xe8\xae\xa4  Esc \xe8\xbf\x94\xe5\x9b\x9e",
            ColorRole::Dim);
    } else {
        m_screen->write(row, 1,
            "\xe2\x86\x91\xe2\x86\x93 \xe5\xaf\xbc\xe8\x88\xaa  Tab \xe5\x90\x91\xe4\xb8\x8b  \xe7\xa9\xba\xe6\xa0\xbc/\xe5\x9b\x9e\xe8\xbd\xa6 \xe7\xa1\xae\xe8\xae\xa4  Esc \xe5\x8f\x96\xe6\xb6\x88",
            ColorRole::Dim);
    }

    m_screen->flush();
}

void SelectPanel::dismiss() {
    // 由调用方负责 end_overlay + reset_buffers
}

} // namespace agent
