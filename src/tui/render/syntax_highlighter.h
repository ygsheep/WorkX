/**
 * @file syntax_highlighter.h
 * @brief 基于 Tree-sitter 的代码语法高亮
 * @details
 *   - 使用 Tree-sitter 对 LLM 返回 / 文件读取的代码做 AST 解析
 *   - 按语法节点类型映射到 SyntaxColor, 输出带 ANSI 颜色的字符串
 *   - 适配 LLM 流式半截代码: Tree-sitter 的 error recovery 保证不完整输入也有 best-effort AST
 *   - 未启用 Tree-sitter 时 (WORKX_HAS_TREE_SITTER 未定义) 退化为 no-op, 不影响渲染管线
 *
 * 集成切入点:
 *   - markdown_renderer.cpp::render_code_block  渲染 LLM markdown 代码块
 *   - FileReadTool / FileEditTool 返回的代码片段 (后续接入)
 */

#pragma once

#include <string>
#include <string_view>

namespace agent {

// ============================================================================
// 颜色角色
// ============================================================================

/// 语法高亮分类 (与具体语言无关)
enum class SyntaxColor {
    Default,       ///< 默认 (不着色)
    Keyword,       ///< 关键字 (if/for/return/class/...)
    String,        ///< 字符串 / 字符字面量
    Comment,       ///< 注释
    Number,        ///< 数字字面量
    Type,          ///< 类型名 (int/MyClass/...)
    Function,      ///< 函数名 (定义 + 调用)
    Punctuation,   ///< 标点 / 运算符
    Property,      ///< 属性 / 字段
    Constant,      ///< 常量 (true/false/null/宏)
    DiffAdd,       ///< diff 新增行 (+)
    DiffDelete,    ///< diff 删除行 (-)
};

/// 获取语法颜色对应的 ANSI 序列
constexpr std::string_view syntax_color_ansi(SyntaxColor c) {
    switch (c) {
        case SyntaxColor::Keyword:     return "\x1b[33m";   // 黄
        case SyntaxColor::String:      return "\x1b[32m";   // 绿
        case SyntaxColor::Comment:     return "\x1b[90m";   // 灰
        case SyntaxColor::Number:      return "\x1b[36m";   // 青
        case SyntaxColor::Type:        return "\x1b[34m";   // 蓝
        case SyntaxColor::Function:    return "\x1b[35m";   // 紫
        case SyntaxColor::Punctuation: return "\x1b[90m";   // 灰
        case SyntaxColor::Property:    return "\x1b[36m";   // 青
        case SyntaxColor::Constant:    return "\x1b[35m";   // 紫
        case SyntaxColor::DiffAdd:     return "\x1b[48;5;22m";   // 深绿底 (diff +, 256色, 接近 Claude Code 风格)
        case SyntaxColor::DiffDelete:  return "\x1b[48;5;52m";   // 深红底 (diff -, 256色, 接近 Claude Code 风格)
        case SyntaxColor::Default:
        default:                       return "\x1b[0m";
    }
}

// ============================================================================
// 核心接口
// ============================================================================

/**
 * @brief 对代码进行语法高亮
 * @param lang 语言标签 (cpp/c/python/bash/js/javascript/ts/rust/go/json/cmake/...)
 *              未知语言或未启用 Tree-sitter 时原样返回
 *              特殊值 "diff": 走专用 diff 渲染 (见 highlight_diff)
 * @param code 原始代码 (可含多行)
 * @return 高亮后的字符串, 每行自包含 ANSI 序列 (不跨行泄漏, 行末带 RESET)
 *
 * @details 输出保证:
 *   - 行内 ANSI 序列成对出现 (start + RESET)
 *   - 每行 \n 之前有 RESET, 下一行需要时重新发出颜色
 *   - 因此可以按 \n 直接 split 后逐行写入 box-drawing 边框, 不会出现颜色泄漏
 */
std::string highlight_code(std::string_view lang, std::string_view code);

/**
 * @brief 对 unified diff 做高亮渲染
 * @param file_lang 文件语言 (cpp/python/...), 用于对 diff 内容做前景色高亮
 *                  若未知或未启用 tree-sitter, 仅画背景色
 * @param diff 原始 diff 文本 (含 --- / +++ / @@ / +line / -line / context)
 * @return 高亮后的字符串
 *
 * @details 渲染规则:
 *   - 文件头 (--- / +++) 和 hunk 头 (@@ ... @@) 完全不输出
 *   - 每行前缀符号 (+/-/space) 不输出, 只输出代码内容
 *   - + 行: 绿色背景 + 内容按 file_lang 前景色高亮
 *   - - 行: 红色背景 + 内容按 file_lang 前景色高亮
 *   - 上下文行: 默认背景 + 内容按 file_lang 前景色高亮
 *   - 即: 背景表示 diff 语义 (增/删), 前景表示代码语义 (关键字/字符串/...)
 */
std::string highlight_diff(std::string_view file_lang, std::string_view diff);

/**
 * @brief 剥离 ANSI 转义序列
 * @details 用于在含 ANSI 的字符串上计算显示宽度 / 做截断
 */
std::string strip_ansi(std::string_view text);

/**
 * @brief 计算含 ANSI 序列文本的显示宽度
 * @details 等价于 display_width(strip_ansi(text)), 但避免临时字符串
 */
int ansi_display_width(std::string_view text);

/**
 * @brief 当前是否启用了 Tree-sitter 语法高亮
 * @return true 表示 highlight_code 会真正做高亮; false 表示 no-op
 */
bool syntax_highlighting_enabled();

} // namespace agent
