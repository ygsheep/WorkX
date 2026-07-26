/**
 * @file utf8_utils.h
 * @brief UTF-8 显示宽度工具
 * @details 解码 UTF-8 字符串为 (字节, 显示宽度) 单元，处理 CJK 双宽/emoji
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <utility>
#include <cstdint>

namespace tui {

/**
 * @brief 解码后的 UTF-8 字符单元
 */
struct Utf8Cell {
    std::string bytes;   ///< UTF-8 字节序列
    int width = 1;       ///< 显示宽度 (0=控制字符, 1=普通, 2=CJK/emoji)
};

/**
 * @brief 计算单个 Unicode codepoint 的显示宽度
 * @param cp Unicode codepoint
 * @return 0=控制字符, 1=普通, 2=CJK/全角/emoji
 * @details 范围参考 Unicode East Asian Width TR11：
 *          - 控制字符 (cp < 0x20) 宽度 0（Tab 由调用方处理）
 *          - ASCII 可打印 (cp < 0x7F) 宽度 1
 *          - 2 字节字符 (0x80-0x7FF) 宽度 1（拉丁扩展等）
 *          - 3 字节 CJK 范围（Hangul Jamo/CJK/Hangul Syllables/CJK Compat/CJK Forms/Fullwidth Forms）宽度 2
 *          - 4 字节字符：Emoji/Misc Symbols/Dingbats/CJK 扩展 E-I 宽度 2，其余 1
 *          - ZWJ 序列（U+200D 连接的 emoji）需 lookahead 判定，本函数仅判定单 codepoint
 */
int char32_width(char32_t cp);

/**
 * @brief 解码 UTF-8 字符串为 (字节, 显示宽度) 列表
 * @details 处理: ASCII(1), 2字节(1), 3字节 CJK/全角(2), 4字节 emoji(2)
 *          控制字符 (除 \t 宽度 4) 宽度为 0
 */
std::vector<Utf8Cell> decode_utf8_cells(std::string_view text);

/**
 * @brief 计算 UTF-8 字符串的显示宽度
 * @return 所有字符显示宽度之和
 */
int display_width(std::string_view text);

/**
 * @brief 按显示宽度截断字符串，截断时追加 "…"
 * @param text 输入文本
 * @param max_width 最大显示宽度 (>= 1)
 * @return 截断后的文本，显示宽度 <= max_width
 */
std::string truncate_to_width(std::string_view text, int max_width);

} // namespace tui
