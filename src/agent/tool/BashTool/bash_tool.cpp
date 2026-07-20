/**
 * @file bash_tool.cpp
 * @brief BashTool 实现
 * @details Shell 执行工具的具体实现
 * @version 1.0.0
 * @date 2026-07
 */

#include "agent/tool/BashTool/bash_tool.h"

namespace agent::tool {

const std::string& BashTool::name() const {
    static const std::string n{"Bash"};
    return n;
}

const std::string& BashTool::description() const {
    static const std::string d{"Executes a shell command and returns output."};
    return d;
}

const std::string& BashTool::prompt() const {
    static const std::string p{
        "Executes a shell command. "
        "Supports timeout and working directory. "
        "Returns stdout, stderr, and exit code."
    };
    return p;
}

nlohmann::json BashTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties", {
            {"command", {{"type", "string"}, {"description", "The shell command to execute"}}},
            {"cwd", {{"type", "string"}, {"description", "Working directory for the command"}}},
            {"timeout", {{"type", "integer"}, {"description", "Timeout in milliseconds"}}}
        }},
        {"required", {"command"}},
        {"additionalProperties", false}
    };
}

ToolResult BashTool::call(
    const nlohmann::json& input,
    const ToolContext& ctx
) {
    // TODO: 实现 shell 执行逻辑
    return ToolResult::error("BashTool not implemented");
}

} // namespace agent::tool
