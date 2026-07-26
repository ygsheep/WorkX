/**
 * @file agent_tool.cpp
 * @brief AgentTool 实现
 * @details 子 Agent 调度工具的具体实现
 * @version 1.0.0
 * @date 2026-07
 */

#include "agent/tool/AgentTool/agent_tool.h"

namespace agent::tool {

const std::string& AgentTool::name() const {
    static const std::string n{"Agent"};
    return n;
}

const std::string& AgentTool::description() const {
    static const std::string d{"Launches a sub-agent to handle a complex task."};
    return d;
}

const std::string& AgentTool::prompt() const {
    static const std::string p{
        "Launches a sub-agent with a specific prompt and tool set. "
        "The sub-agent runs independently and returns its result."
    };
    return p;
}

nlohmann::json AgentTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties", {
            {"prompt", {{"type", "string"}, {"description", "The task prompt for the sub-agent"}}},
            {"tools", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Allowed tools for the sub-agent"}}}
        }},
        {"required", {"prompt"}},
        {"additionalProperties", false}
    };
}

ToolResult AgentTool::call(
    const nlohmann::json& input,
    const ToolContext& ctx
) const {
    // TODO: 实现子 Agent 调度逻辑
    return ToolResult::error("AgentTool not implemented");
}

} // namespace agent::tool
