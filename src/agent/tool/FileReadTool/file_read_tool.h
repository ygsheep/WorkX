/**
 * @file file_read_tool.h
 * @brief FileReadTool — 文件读取工具
 * @details 读取文本/图片/PDF/Notebook 文件内容，支持行范围、去重优化
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include "agent/tool/itool.h"
#include "agent/tool/types.h"

namespace agent::tool {

/// @brief FileReadTool — 文件读取工具
///
/// 支持读取多种格式的文件：
/// - text: 普通文本文件（支持行号、偏移量）
/// - image: 图片文件（PNG/JPG 等，Base64 编码）
/// - notebook: Jupyter Notebook（.ipynb）
/// - pdf: PDF 文件（支持分页读取）
class FileReadTool : public ITool {
public:
    /// @brief 工具名称
    const std::string& name() const override;

    /// @brief 工具描述
    const std::string& description() const override;

    /// @brief 工具提示词
    const std::string& prompt() const override;

    /// @brief 参数 JSON Schema
    nlohmann::json input_schema() const override;

    /// @brief 执行文件读取
    ToolResult call(
        const nlohmann::json& input,
        const ToolContext& ctx
    ) override;
};

} // namespace agent::tool
