/**
 * @file utf8_utils.cpp
 * @brief UTF-8 显示宽度工具实现
 */

#include "tui/utils/utf8_utils.h"

namespace agent {

std::vector<Utf8Cell> decode_utf8_cells(std::string_view text) {
    std::vector<Utf8Cell> result;
    size_t i = 0;
    while (i < text.size()) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        size_t len = 1;
        int width = 1;

        if ((c & 0x80) == 0) {
            // ASCII
            len = 1;
            if (c < 0x20 && c != '\t') width = 0;  // 控制字符宽度为 0
        } else if ((c & 0xE0) == 0xC0) {
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
            // CJK / 全角字符宽度为 2
            if (i + 2 < text.size()) {
                char32_t cp = (static_cast<char32_t>(c & 0x0F) << 12)
                    | (static_cast<char32_t>(static_cast<unsigned char>(text[i+1]) & 0x3F) << 6)
                    | static_cast<char32_t>(static_cast<unsigned char>(text[i+2]) & 0x3F);
                if (cp >= 0x1100 && (
                    (cp <= 0x115F) ||                                   // Hangul Jamo
                    (cp >= 0x2E80 && cp <= 0xA4CF && cp != 0x303F) ||   // CJK
                    (cp >= 0xAC00 && cp <= 0xD7A3) ||                   // Hangul Syllables
                    (cp >= 0xF900 && cp <= 0xFAFF) ||                   // CJK Compat
                    (cp >= 0xFE30 && cp <= 0xFE6F) ||                   // CJK Forms
                    (cp >= 0xFF01 && cp <= 0xFF60) ||                   // Fullwidth Forms
                    (cp >= 0xFFE0 && cp <= 0xFFE6))) {
                    width = 2;
                }
                // Box Drawing / Block Elements 等宽度为 1
            }
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
            width = 2;  // Emoji 通常宽度为 2
        }

        if (i + len > text.size()) len = text.size() - i;

        Utf8Cell cell;
        cell.bytes = std::string(text.substr(i, len));
        cell.width = width;
        result.push_back(std::move(cell));
        i += len;
    }
    return result;
}

int display_width(std::string_view text) {
    int total = 0;
    for (const auto& cell : decode_utf8_cells(text)) {
        total += cell.width;
    }
    return total;
}

std::string truncate_to_width(std::string_view text, int max_width) {
    if (max_width <= 0) return "";

    int full_width = display_width(text);
    if (full_width <= max_width) return std::string(text);

    // max_width == 1: can only fit ellipsis
    if (max_width == 1) return "\xe2\x80\xa6";

    // max_width >= 2: try to fit chars + ellipsis
    int target_with_ellipsis = max_width - 1;
    auto cells = decode_utf8_cells(text);

    int current_width = 0;
    std::string result;
    for (const auto& cell : cells) {
        if (current_width + cell.width > target_with_ellipsis) break;
        result += cell.bytes;
        current_width += cell.width;
    }
    if (!result.empty()) {
        result += "\xe2\x80\xa6";
        return result;
    }

    // No chars fit with ellipsis (CJK edge case): fit without ellipsis
    current_width = 0;
    result.clear();
    for (const auto& cell : cells) {
        if (current_width + cell.width > max_width) break;
        result += cell.bytes;
        current_width += cell.width;
    }
    return result;
}

} // namespace workx
