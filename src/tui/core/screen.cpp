/**
 * @file screen.cpp
 * @brief 差分渲染屏幕缓冲实现
 */

#include "tui/core/screen.h"
#include "tui/core/terminal.h"
#include "tui/utils/utf8_utils.h"
#include <algorithm>
#include <format>

namespace agent {

Screen::Screen(Terminal* terminal)
    : m_terminal(terminal)
{
    resize(80, 24);  // 默认终端尺寸
}

void Screen::resize(int w, int h) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    m_width = w;
    m_height = h;

    m_lines.resize(h);
    m_previous.resize(h);

    for (auto& line : m_lines) {
        line.cells.resize(w);
        line.dirty = true;
    }
    for (auto& line : m_previous) {
        line.cells.resize(w);
        line.dirty = true;
    }
}

void Screen::write(int row, int col, const std::string& text, ColorRole color) {
    ensure_lines(row);

    auto cells = decode_utf8_cells(text);
    int c = col;

    for (const auto& uc : cells) {
        if (c >= m_width) break;

        const std::string& bytes = uc.bytes;
        int w = uc.width;

        auto& cell = m_lines[row].cells[c];
        if (cell.ch != bytes || cell.color != color || cell.width != w) {
            cell.ch = bytes;
            cell.color = color;
            cell.width = w;
            m_lines[row].dirty = true;
        }

        // 宽字符：在下一个 cell 留空标记（防止覆盖）
        c++;
        if (w == 2 && c < m_width) {
            auto& next = m_lines[row].cells[c];
            // 用空字符串作为宽字符延续标记（width=0 + ch=""）
            if (!next.ch.empty() || next.color != color || next.width != 0) {
                next.ch = "";  // 标记为宽字符的延续
                next.color = color;
                next.width = 0;
                m_lines[row].dirty = true;
            }
            c++;
        }
    }
}

void Screen::fill(int row, int col, int width, char c) {
    ensure_lines(row);

    std::string s(1, c);
    for (int i = 0; i < width; i++) {
        int cc = col + i;
        if (cc >= m_width) break;
        auto& cell = m_lines[row].cells[cc];
        if (cell.ch != s || cell.color != ColorRole::Default || cell.width != 1) {
            cell.ch = s;
            cell.color = ColorRole::Default;
            cell.width = 1;
            m_lines[row].dirty = true;
        }
    }
}

void Screen::draw_box(int row, int col, int width, int height, const std::string& title) {
    // 参数校验：宽度 >=4，高度 >=3，且不超出缓冲区
    if (width < 4 || height < 3 || row + height > m_height || col + width > m_width) return;

    int inner_w = width - 2;   // 内部宽度（不含左右边框）
    int inner_h = height - 2;  // 内部高度（不含上下边框）

    // 顶行: ╔═ title ═╗
    write(row, col, "\xe2\x95\x94", ColorRole::StatusBar);  // ╔
    write(row, col + 1, " " + title + " ", ColorRole::StatusBar);
    int fill_start = col + 1 + static_cast<int>(title.size()) + 1;
    for (int i = fill_start; i < col + inner_w; i++) {
        write(row, i, "\xe2\x95\x90", ColorRole::StatusBar);  // ═
    }
    write(row, col + inner_w, "\xe2\x95\x97", ColorRole::StatusBar);  // ╗

    // 中间行：用 inner_h（高度）控制垂直循环，修复原代码用 inner_w 的 bug
    for (int r = row + 1; r < row + 1 + inner_h; r++) {
        write(r, col, "\xe2\x95\x91", ColorRole::StatusBar);           // ║
        write(r, col + inner_w, "\xe2\x95\x91", ColorRole::StatusBar); // ║
    }

    // 底行
    int bottom = row + height - 1;
    write(bottom, col, "\xe2\x95\x9a", ColorRole::StatusBar);  // ╚
    for (int i = col + 1; i < col + inner_w; i++) {
        write(bottom, i, "\xe2\x95\x90", ColorRole::StatusBar);  // ═
    }
    write(bottom, col + inner_w, "\xe2\x95\x9d", ColorRole::StatusBar);  // ╝
}

void Screen::clear() {
    for (auto& line : m_lines) {
        for (auto& cell : line.cells) {
            cell.ch = " ";
            cell.color = ColorRole::Default;
            cell.width = 1;
        }
        line.dirty = true;
    }
}

void Screen::clear_terminal() {
    // 直接向终端发送全屏清空 + 光标归位
    m_terminal->write("\x1b[2J\x1b[H");
    // 重置缓冲区状态
    for (auto& line : m_lines) {
        for (auto& cell : line.cells) {
            cell.ch = " ";
            cell.color = ColorRole::Default;
            cell.width = 1;
        }
        line.dirty = false;
    }
    // 重置上一帧，避免下次 flush 写出残留 diff
    for (auto& line : m_previous) {
        for (auto& cell : line.cells) {
            cell.ch = " ";
            cell.color = ColorRole::Default;
            cell.width = 1;
        }
        line.dirty = false;
    }
}

void Screen::reset_buffers() {
    for (auto& line : m_lines) {
        for (auto& cell : line.cells) {
            cell.ch = " ";
            cell.color = ColorRole::Default;
            cell.width = 1;
        }
        line.dirty = false;
    }
    for (auto& line : m_previous) {
        for (auto& cell : line.cells) {
            cell.ch = " ";
            cell.color = ColorRole::Default;
            cell.width = 1;
        }
        line.dirty = false;
    }
}

void Screen::flush() {
    for (int row = 0; row < m_height; row++) {
        if (m_lines[row].dirty) {
            // 检查与上一帧是否真正不同
            const auto& cur = m_lines[row];
            const auto& prev = m_previous[row];

            bool changed = false;
            for (int c = 0; c < m_width; c++) {
                if (cur.cells[c].ch != prev.cells[c].ch ||
                    cur.cells[c].color != prev.cells[c].color ||
                    cur.cells[c].width != prev.cells[c].width) {
                    changed = true;
                    break;
                }
            }

            if (changed) {
                render_line(row, cur);
                // 更新 previous
                for (int c = 0; c < m_width; c++) {
                    m_previous[row].cells[c] = cur.cells[c];
                }
            }
            m_lines[row].dirty = false;
            m_previous[row].dirty = false;
        }
    }
}

void Screen::render_line(int row, const ScreenLine& line) {
    // ANSI 绝对定位: \x1b[<row+1>;1H
    m_terminal->write(std::format("\x1b[{};1H", row + 1));

    // 分组输出：连续相同颜色的字符一次性写入
    ColorRole current_color = line.cells[0].color;
    std::string segment;

    auto flush_segment = [&]() {
        if (!segment.empty()) {
            if (current_color != ColorRole::Default) {
                m_terminal->set_color(current_color);
            }
            m_terminal->write(segment);
            if (current_color != ColorRole::Default) {
                m_terminal->reset_color();
            }
            segment.clear();
        }
    };

    for (int c = 0; c < m_width; c++) {
        const auto& cell = line.cells[c];

        // 宽字符延续位：width==0 表示由前一个 cell 的宽字符覆盖显示，跳过
        // 仅用 width==0 判定，ch 不参与（避免与 "\0"/" " 比较的语义歧义）
        if (cell.width == 0) {
            // 颜色变化仍需 flush
            if (cell.color != current_color) {
                flush_segment();
                current_color = cell.color;
            }
            continue;
        }

        if (cell.color != current_color) {
            flush_segment();
            current_color = cell.color;
        }
        segment += cell.ch;
    }
    flush_segment();

    // 清除行尾多余内容
    m_terminal->write("\x1b[K");
}

void Screen::ensure_lines(int row) {
    if (row < m_height) return;

    // 自动扩展缓冲区
    int new_h = std::max(row + 1, m_height * 2);
    resize(m_width, new_h);
}

} // namespace agent
