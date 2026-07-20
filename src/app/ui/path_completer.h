/**
 * @file path_completer.h
 * @brief 文件路径 Tab 补全
 * @details 匹配给定前缀的文件/目录，用于输入行路径补全
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <utility>

namespace agent {

/// @brief 文件路径补全：匹配给定前缀的文件/目录
/// @param prefix 输入行前缀
/// @param cursor_pos 光标位置
/// @return 补全结果列表（补全后的完整行 + 新光标位置）
std::vector<std::pair<std::string, size_t>> complete_file_path(
    std::string_view prefix, size_t cursor_pos);

} // namespace agent
