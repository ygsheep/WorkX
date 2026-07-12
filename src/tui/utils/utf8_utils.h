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

namespace agent {

/**
 * @brief 解码后的 UTF-8 字符单元
 */
struct Utf8Cell {
    std::string bytes;   ///< UTF-8 字节序列
    int width = 1;       ///< 显示宽度 (0=控制字符, 1=普通, 2=CJK/emoji)
};

/**
 * @brief 解码 UTF-8 字符串为 (字节, 显示宽度) 列表
 * @details 处理: ASCII(1), 2字节(1), 3字节 CJK/全角(2), 4字节 emoji(2)
 *          控制字符 (除 \t) 宽度为 0
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

} // namespace workx
