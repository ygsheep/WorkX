/**
 * @file file_edit_tool.h
 * @brief FileEditTool — 文件编辑工具
 * @details 通过字符串匹配替换文件内容，支持单次/全部替换
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include "agent/tool/itool.h"
#include "agent/tool/types.h"

namespace agent::tool {

/// @brief FileEditTool — 文件编辑工具
///
/// 通过精确字符串匹配替换文件内容：
/// - replace_all=false: 替换第一个匹配
/// - replace_all=true: 替换所有匹配
class FileEditTool : public ITool {
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
