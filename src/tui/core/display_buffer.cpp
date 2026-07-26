/**
 * @file display_buffer.cpp
 * @brief 聊天滚动区域物理行镜像实现
 */

#include "tui/core/display_buffer.h"
#include "tui/utils/utf8_utils.h"

#include <algorithm>

namespace tui {

DisplayBuffer::DisplayBuffer(int capacity) {
    m_capacity = capacity > 0 ? capacity : 1;
    m_rows.resize(m_capacity);
}

void DisplayBuffer::set_width(int width) {
    if (width > 0) m_width = width;
}

void DisplayBuffer::set_height(int height) {
    if (height > 0) m_height = height;
}

void DisplayBuffer::clear_all() {
    for (auto& r : m_rows) r.clear();
    m_total_rows = 0;
    m_head = 0;
    m_row_builder.clear();
    m_row_sgr.clear();
    m_active_sgr.clear();
    m_col = 0;
    m_text_run.clear();
    m_csi_params.clear();
    m_parse_state = ParseState::Normal;
}

void DisplayBuffer::finalize_row() {
    std::string row = m_row_sgr + m_row_builder;
    m_rows[m_head] = std::move(row);
    m_head = (m_head + 1) % m_capacity;
    m_total_rows++;
}

void DisplayBuffer::flush_text_run() {
    if (m_text_run.empty()) return;
    handle_text(m_text_run);
    m_text_run.clear();
}

void DisplayBuffer::handle_text(std::string_view text) {
    auto cells = decode_utf8_cells(text);
    for (const auto& cell : cells) {
        // 折行：当前列已非空且加入本字符会溢出
        if (m_col > 0 && m_col + cell.width > m_width) {
            finalize_row();
            m_row_sgr = m_active_sgr;
            m_row_builder.clear();
            m_col = 0;
        }
        m_row_builder += cell.bytes;
        m_col += cell.width;
    }
}

void DisplayBuffer::handle_csi(std::string_view params, char final_byte) {
    if (final_byte == 'm') {
        // SGR：内联追加到当前行 builder，并更新活跃 SGR
        std::string raw = "\x1b[";
        raw += params;
        raw += 'm';
        m_row_builder += raw;
        if (params.empty() || params == "0") {
            // 全 reset：重建活跃 SGR 基线
            m_active_sgr = "\x1b[0m";
        } else {
            m_active_sgr += raw;
            // 防止异常累积（理论上 set_color 总先 reset，不会触发）
            if (m_active_sgr.size() > 64) {
                m_active_sgr = "\x1b[0m" + raw;
            }
        }
    } else if (final_byte == 'J') {
        if (params == "2") {
            clear_all();
        }
        // 其他 J（0/1）忽略
    }
    // K / H / f / r / s / u 等均忽略（顺序模型或恢复时另行处理）
}

void DisplayBuffer::feed(std::string_view bytes) {
    for (size_t i = 0; i < bytes.size(); ++i) {
        unsigned char b = static_cast<unsigned char>(bytes[i]);
        switch (m_parse_state) {
            case ParseState::Normal:
                if (b == 0x1b) {
                    flush_text_run();
                    m_parse_state = ParseState::Esc;
                } else if (b == '\n') {
                    flush_text_run();
                    finalize_row();
                    m_row_sgr = m_active_sgr;
                    m_row_builder.clear();
                    m_col = 0;
                } else if (b == '\r') {
                    flush_text_run();
                    m_col = 0;
                } else {
                    m_text_run += static_cast<char>(b);
                }
                break;
            case ParseState::Esc:
                if (b == '[') {
                    m_parse_state = ParseState::Csi;
                    m_csi_params.clear();
                } else {
                    // 非 CSI 转义（如 \x1bc）：忽略该字节，回到 Normal
                    m_parse_state = ParseState::Normal;
                }
                break;
            case ParseState::Csi:
                // 终止字节范围 0x40-0x7E
                if (b >= 0x40 && b <= 0x7E) {
                    handle_csi(m_csi_params, static_cast<char>(b));
                    m_csi_params.clear();
                    m_parse_state = ParseState::Normal;
                } else {
                    m_csi_params += static_cast<char>(b);
                }
                break;
        }
    }
}

std::vector<std::string> DisplayBuffer::snapshot(int top_row, int bottom_row) const {
    std::vector<std::string> result;
    if (top_row > bottom_row) {
        return result;
    }
    int scroll_h = m_height - 3;
    if (scroll_h < 1) scroll_h = 1;

    // Content is bottom-aligned in the scroll region: write() positions the
    // cursor to scroll_bottom before the first write, so after n lines (n <
    // scroll_h) the content occupies the bottom n rows. Unified mapping:
    //   physical = screen_row + total_rows - scroll_h
    for (int r = top_row; r <= bottom_row; ++r) {
        int physical = r + m_total_rows - scroll_h;
        if (physical < 1 || physical > m_total_rows) {
            result.emplace_back();
        } else if (m_total_rows > m_capacity && physical <= m_total_rows - m_capacity) {
            result.emplace_back();
        } else {
            int idx = (physical - 1) % m_capacity;
            result.push_back(m_rows[idx]);
        }
    }
    return result;
}

} // namespace tui
