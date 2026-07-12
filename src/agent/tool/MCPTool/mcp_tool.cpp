/**
 * @file mcp_tool.cpp
 * @brief MCPTool 实现
 * @details MCP 工具调用的具体实现
 * @version 1.0.0
 * @date 2026-07
 */

#include "agent/tool/MCPTool/mcp_tool.h"

namespace agent::tool {

const std::string& MCPTool::name() const {
    static const std::string n{"MCP"};
    return n;
}

const std::string& MCPTool::description() const {
    static const std::string d{"Invokes an external tool via Model Context Protocol."};
    return d;
}

const std::string& MCPTool::prompt() const {
    static const std::string p{
        "Invokes an external tool registered via MCP. "
        "Passes through input parameters and returns results."
    };
    return p;
}

nlohmann::json MCPTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties", {
            {"server", {{"type", "string"}, {"description", "MCP server name"}}},
            {"tool", {{"type", "string"}, {"description", "Tool name on the MCP server"}}},
            {"input", {{"type", "object"}, {"description", "Tool input parameters"}}}
        }},
        {"required", {"server", "tool"}}
    };
}

ToolResult MCPTool::call(
    const nlohmann::json& input,
    const ToolContext& ctx
) {
    // TODO: 实现 MCP 工具调用逻辑
    return ToolResult::error("MCPTool not implemented");
}

} // namespace agent::tool
