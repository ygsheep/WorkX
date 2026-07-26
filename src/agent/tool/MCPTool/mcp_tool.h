/**
 * @file mcp_tool.h
 * @brief MCPTool — MCP 工具调用
 * @details 调用 Model Context Protocol 外部工具
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include "agent/tool/itool.h"

namespace agent::tool {

/// @brief MCPTool — MCP 工具调用
///
/// 通过 Model Context Protocol 调用外部工具：
/// - 支持 MCP server 注册的工具
/// - 透传输入参数和返回结果
class MCPTool : public ITool {
public:
    const std::string& name() const override;
    const std::string& description() const override;
    const std::string& prompt() const override;
    nlohmann::json input_schema() const override;

    ToolResult call(
        const nlohmann::json& input,
        const ToolContext& ctx
    ) const override;
};

} // namespace agent::tool
