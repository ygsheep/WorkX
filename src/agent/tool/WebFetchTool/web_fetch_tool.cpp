/**
 * @file web_fetch_tool.cpp
 * @brief WebFetchTool 实现
 * @details 网页抓取工具的具体实现
 * @version 1.0.0
 * @date 2026-07
 */

#include "agent/tool/WebFetchTool/web_fetch_tool.h"

namespace agent::tool {

const std::string& WebFetchTool::name() const {
    static const std::string n{"WebFetch"};
    return n;
}

const std::string& WebFetchTool::description() const {
    static const std::string d{"Fetches a URL and converts content to Markdown."};
    return d;
}

const std::string& WebFetchTool::prompt() const {
    static const std::string p{
        "Fetches a URL and converts HTML to Markdown. "
        "Supports a prompt to extract specific information."
    };
    return p;
}

nlohmann::json WebFetchTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties", {
            {"url", {{"type", "string"}, {"description", "The URL to fetch"}}},
            {"prompt", {{"type", "string"}, {"description", "Optional prompt to extract specific information"}}}
        }},
        {"required", {"url"}},
        {"additionalProperties", false}
    };
}

ResultV2<ToolResult> WebFetchTool::call(
    const nlohmann::json& /*input*/,
    const ToolContext& /*ctx*/
) const {
    // TODO: 实现网页抓取逻辑
    return ResultV2<ToolResult>::err(Error::Code::NotImplemented, "WebFetchTool not implemented");
}

} // namespace agent::tool
