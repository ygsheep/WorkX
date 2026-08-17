/**
 * @file syntax_highlight.h
 * @brief 轻量代码高亮（常见语言子集，行内分词，无外部依赖）
 * @details 不引入 tree-sitter（计划 §6 列为后续增强）：按行独立分词，
 *          识别 注释/字符串/数字/标识符（关键字/内置类型）并着色。
 *          不支持的或未标注的语言返回普通文本行，行为与之前一致。
 */

#pragma once

#include <string_view>

#include <ftxui/dom/elements.hpp>

namespace ftxtui {

/// @brief 高亮一行代码（按语言；未知语言原样输出）
/// @param line 单行代码（不含换行）
/// @param lang 代码块标注语言（如 "cpp" / "python" / "js" / "bash"）
ftxui::Element highlight_code_line(std::string_view line, std::string_view lang);

}  // namespace ftxtui
