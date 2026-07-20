/**
 * @file utf8_utils.cpp
 * @brief UTF-8 显示宽度工具实现
 */

#include "tui/utils/utf8_utils.h"

#include <numeric>

namespace agent {

int char32_width(char32_t cp) {
    // 控制字符宽度 0（Tab 由调用方单独处理）
    if (cp < 0x20) return 0;

    // ASCII 可打印宽度 1
    if (cp < 0x7F) return 1;

    // 删除字符 (DEL) 宽度 0
    if (cp == 0x7F) return 0;

    // 2 字节字符 (0x80-0x7FF)：拉丁扩展等，宽度 1
    if (cp <= 0x7FF) return 1;

    // 3 字节字符 (0x800-0xFFFF)
    if (cp <= 0xFFFF) {
        // Emoji / 符号范围宽度 2（BMP 内的 3 字节 emoji）
        // 注意：这些 codepoint 在 BMP 内（3 字节 UTF-8），原代码误放在 4 字节分支
        if (cp >= 0x2600 && cp <= 0x26FF) return 2;    // Misc Symbols (☀☁☂☃...)
        if (cp >= 0x2700 && cp <= 0x27BF) return 2;    // Dingbats (✅✗❌✋...)
        if (cp >= 0x2B00 && cp <= 0x2BFF) return 2;    // Misc Symbols & Arrows (⭐⬆⬇...)

        // CJK / 全角字符宽度 2（参考 Unicode East Asian Width TR11）
        if (cp >= 0x1100 && (
            (cp <= 0x115F) ||                                   // Hangul Jamo
            (cp >= 0x2E80 && cp <= 0xA4CF && cp != 0x303F) ||   // CJK Radicals / Kangxi / CJK Symbols
            (cp >= 0xAC00 && cp <= 0xD7A3) ||                   // Hangul Syllables
            (cp >= 0xF900 && cp <= 0xFAFF) ||                   // CJK Compatibility Ideographs
            (cp >= 0xFE30 && cp <= 0xFE6F) ||                   // CJK Compatibility Forms
            (cp >= 0xFF01 && cp <= 0xFF60) ||                   // Fullwidth Forms
            (cp >= 0xFFE0 && cp <= 0xFFE6))) {                   // Fullwidth Signs
            return 2;
        }
        // 其余 3 字节字符（含 Box Drawing / Block Elements / 拉丁扩展等）宽度 1
        return 1;
    }

    // 4 字节字符 (>= 0x10000)
    // Emoji 与符号范围宽度 2
    if (cp >= 0x1F300 && cp <= 0x1FAFF) return 2;  // Emoji & Symbols
    if (cp >= 0x1F000 && cp <= 0x1F02F) return 2;  // Mahjong Tiles
    if (cp >= 0x1F0A0 && cp <= 0x1F0FF) return 2;  // Playing Cards

    // CJK 扩展 E-I 宽度 2
    if (cp >= 0x2B700 && cp <= 0x2B73F) return 2;  // CJK Ext E
    if (cp >= 0x2B740 && cp <= 0x2B81F) return 2;  // CJK Ext F
    if (cp >= 0x2B820 && cp <= 0x2CEAF) return 2;  // CJK Ext G
    if (cp >= 0x2CEB0 && cp <= 0x2EBEF) return 2;  // CJK Ext H
    if (cp >= 0x30000 && cp <= 0x3134F) return 2;  // CJK Ext I

    // 其余 4 字节字符宽度 1
    return 1;
}

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
            if (c < 0x20) {
                // Tab 宽度 4（保守值，与多数编辑器一致），其他控制字符宽度 0
                width = (c == '\t') ? 4 : 0;
            }
        } else if ((c & 0xE0) == 0xC0) {
            len = 2;
            // 2 字节字符：解码 codepoint 后调用 char32_width
            if (i + 1 < text.size()) {
                char32_t cp = (static_cast<char32_t>(c & 0x1F) << 6)
                    | static_cast<char32_t>(static_cast<unsigned char>(text[i+1]) & 0x3F);
                width = char32_width(cp);
            }
        } else if ((c & 0xF0) == 0xE0) {
            len = 3;
            // 3 字节字符：解码 codepoint 后调用 char32_width
            if (i + 2 < text.size()) {
                char32_t cp = (static_cast<char32_t>(c & 0x0F) << 12)
                    | (static_cast<char32_t>(static_cast<unsigned char>(text[i+1]) & 0x3F) << 6)
                    | static_cast<char32_t>(static_cast<unsigned char>(text[i+2]) & 0x3F);
                width = char32_width(cp);
            }
        } else if ((c & 0xF8) == 0xF0) {
            len = 4;
            // 4 字节字符：解码 codepoint 后调用 char32_width
            if (i + 3 < text.size()) {
                char32_t cp = (static_cast<char32_t>(c & 0x07) << 18)
                    | (static_cast<char32_t>(static_cast<unsigned char>(text[i+1]) & 0x3F) << 12)
                    | (static_cast<char32_t>(static_cast<unsigned char>(text[i+2]) & 0x3F) << 6)
                    | static_cast<char32_t>(static_cast<unsigned char>(text[i+3]) & 0x3F);
                width = char32_width(cp);
            } else {
                width = 2;  // 截断的 4 字节序列回退
            }
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
    auto cells = decode_utf8_cells(text);
    return std::accumulate(cells.begin(), cells.end(), 0,
        [](int acc, const auto& cell) { return acc + cell.width; });
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

} // namespace agent
