/**
 * @file grep_tool.cpp
 * @brief GrepTool 实现
 * @details 内容搜索工具的具体实现
 * @version 1.0.0
 * @date 2026-07
 */

#include "agent/tool/GrepTool/grep_tool.h"

namespace agent::tool {

const std::string& GrepTool::name() const {
    static const std::string n{"Grep"};
    return n;
}

const std::string& GrepTool::description() const {
    static const std::string d{"Searches file contents using regex or literal patterns."};
    return d;
}

const std::string& GrepTool::prompt() const {
    static const std::string p{
        "Searches file contents for matching patterns. "
        "Supports regex and literal matching. "
        "Returns file path, line number, and line content."
    };
    return p;
}

nlohmann::json GrepTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties", {
            {"pattern", {{"type", "string"}, {"description", "The search pattern (regex or literal)"}}},
            {"path", {{"type", "string"}, {"description", "The path to search in"}}},
            {"case_insensitive", {{"type", "boolean"}, {"description", "Ignore case"}, {"default", false}}},
            {"regex", {{"type", "boolean"}, {"description", "Treat pattern as regex"}, {"default", true}}}
        }},
        {"required", {"pattern"}},
        {"additionalProperties", false}
    };
}

ResultV2<ToolResult> GrepTool::call(
    const nlohmann::json& /*input*/,
    const ToolContext& /*ctx*/
) const {
    // TODO: 实现 grep 搜索逻辑
    return ResultV2<ToolResult>::err(Error::Code::NotImplemented, "GrepTool not implemented");
}

} // namespace agent::tool
