/**
 * @file file_write_tool.h
 * @brief FileWriteTool — 文件写入工具
 * @details 创建或覆盖文件内容，生成 diff
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include "agent/tool/itool.h"
#include "agent/tool/types.h"

namespace agent::tool {

/// @brief FileWriteTool — 文件写入工具
///
/// 创建新文件或覆盖现有文件内容：
/// - create: 文件不存在时创建
/// - update: 文件存在时覆盖并生成 diff
class FileWriteTool : public ITool {
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
