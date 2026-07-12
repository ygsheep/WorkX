/**
 * @file grep_tool.h
 * @brief GrepTool — 内容搜索工具
 * @details 在文件中搜索匹配的行，支持正则和字面量
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include "agent/tool/itool.h"
#include "agent/tool/types.h"

namespace agent::tool {

/// @brief GrepTool — 内容搜索工具
///
/// 在指定路径下搜索文件内容：
/// - 支持正则表达式和字面量匹配
/// - 支持大小写敏感/忽略
/// - 返回匹配的文件名、行号、行内容
class GrepTool : public ITool {
public:
    const std::string& name() const override;
    const std::string& description() const override;
    const std::string& prompt() const override;
    nlohmann::json input_schema() const override;

    ToolResult call(
        const nlohmann::json& input,
        const ToolContext& ctx
    ) override;
};

} // namespace agent::tool
