/**
 * @file syntax_highlight.h
 * @brief 代码语法高亮（tree-sitter AST + 关键字回退）
 * @details 启用 tree-sitter（WORKX_HAS_TREE_SITTER）时，代码块走整块 AST
 *          解析（highlight_code_block），按语法节点类型着色；未启用或语言
 *          无 grammar 时回退到行内关键字分词（highlight_code_line）。
 *          不支持的或未标注的语言返回普通文本行，行为与之前一致。
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <ftxui/dom/elements.hpp>

namespace ftxtui {

/// @brief 高亮一行代码（按语言；未知语言原样输出）
/// @param line 单行代码（不含换行）
/// @param lang 代码块标注语言（如 "cpp" / "python" / "js" / "bash"）
ftxui::Element highlight_code_line(std::string_view line, std::string_view lang);

/// @brief 高亮整块代码（tree-sitter 解析整块，返回每行一个 Element）
/// @param lines 代码块各行（不含换行）
/// @param lang 代码块标注语言
/// @return 与 lines 等长的 Element 列表；tree-sitter 不可用或语言不支持时
///         逐行回退到 highlight_code_line
std::vector<ftxui::Element> highlight_code_block(
    const std::vector<std::string>& lines, std::string_view lang);

}  // namespace ftxtui
