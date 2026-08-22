/**
 * @file read_mcp_resource_tool.cpp
 * @brief ReadMcpResourceTool 实现
 * @details 调用 resources/read 读取指定资源内容：
 *          - 文本内容（text）直接输出
 *          - 二进制内容（blob，base64）标注长度，不直接解码
 * @version 1.0.0
 * @date 2026-08
 */

#include "agent/tool/ReadMcpResourceTool/read_mcp_resource_tool.h"

#include <sstream>

namespace agent::tool {

ReadMcpResourceTool::ReadMcpResourceTool(
    std::shared_ptr<mcp::McpClientManager> manager)
    : m_manager(std::move(manager)) {}

const std::string& ReadMcpResourceTool::name() const {
    static const std::string n{"ReadMcpResourceTool"};
    return n;
}

const std::string& ReadMcpResourceTool::description() const {
    static const std::string d{
        "Reads a specific MCP resource by URI from a connected MCP server."};
    return d;
}

const std::string& ReadMcpResourceTool::prompt() const {
    static const std::string p{
        "ReadMcpResourceTool：读取指定 MCP server 的资源内容。\n"
        "用法：{\"server\": \"<server名>\", \"uri\": \"<资源URI>\"}\n"
        "uri 来自 ListMcpResourcesTool 的输出。"};
    return p;
}

nlohmann::json ReadMcpResourceTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties", {
            {"server", {{"type", "string"}, {"description", "MCP server name"}}},
            {"uri", {{"type", "string"}, {"description", "Resource URI to read"}}}
        }},
        {"required", {"server", "uri"}},
        {"additionalProperties", false}
    };
}

ResultV2<ToolResult> ReadMcpResourceTool::call(
    const nlohmann::json& input,
    const ToolContext& /*ctx*/
) const {
    if (!input.contains("server") || !input.at("server").is_string()) {
        return ResultV2<ToolResult>::err(
            Error::Code::InvalidInput, "ReadMcpResourceTool 需要字符串参数 server");
    }
    if (!input.contains("uri") || !input.at("uri").is_string()) {
        return ResultV2<ToolResult>::err(
            Error::Code::InvalidInput, "ReadMcpResourceTool 需要字符串参数 uri");
    }
    if (!m_manager) {
        return ResultV2<ToolResult>::err(
            Error::Code::InternalError, "MCP 连接管理器未初始化");
    }

    const std::string server = input.at("server").get<std::string>();
    const std::string uri = input.at("uri").get<std::string>();

    auto client = m_manager->get_client(server);
    if (!client) {
        return ResultV2<ToolResult>::err(
            Error::Code::ResourceNotFound,
            "MCP server 不存在或未连接: " + server);
    }

    auto result = client->read_resource(uri);
    if (result.is_err()) {
        return ResultV2<ToolResult>::err(
            Error::Code::ToolExecutionFailed,
            "读取 MCP 资源失败: " + result.error().message,
            "server=" + server + "; uri=" + uri);
    }

    std::ostringstream out;
    out << "# MCP 资源内容: " << server << " / " << uri << "\n";
    const auto& contents = result.value();
    if (contents.empty()) {
        out << "（资源为空）\n";
    }
    for (const auto& c : contents) {
        if (!c.mime_type.empty()) {
            out << "mimeType: " << c.mime_type << "\n";
        }
        if (!c.text.empty()) {
            out << "---\n" << c.text << "\n";
        } else if (!c.blob.empty()) {
            out << "[二进制内容] blob_len=" << c.blob.size()
                << "（base64，未解码）\n";
        }
    }
    return ResultV2<ToolResult>::ok(ToolResult::ok(out.str()));
}

} // namespace agent::tool
