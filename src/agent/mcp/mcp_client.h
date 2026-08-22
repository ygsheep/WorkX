/**
 * @file mcp_client.h
 * @brief MCP client（Issue #27）
 * @details 单个 MCP server 的 JSON-RPC 客户端：
 *          - 协议协商：server/discover（2.0 无状态）→ initialize（1.x 握手）回退
 *          - 工具：tools/list、tools/call
 *          - 资源：resources/list、resources/read
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/mcp/mcp_config.h"
#include "agent/mcp/mcp_transport.h"
#include "agent/mcp/mcp_types.h"
#include "core/utils/result_v2.h"

namespace agent::mcp {

/// @brief MCP client：连接单个 server，提供工具/资源调用
class McpClient {
public:
    McpClient();
    ~McpClient();

    McpClient(const McpClient&) = delete;
    McpClient& operator=(const McpClient&) = delete;

    /// @brief 连接 server（协议协商：discover → initialize 回退）
    /// @param cfg server 配置
    /// @param timeout_ms 单次请求超时
    /// @return ok: 已连接；err: 启动失败/协商失败
    ResultV2<void> connect(const McpServerConfig& cfg, int timeout_ms = 15000);

    /// @brief 断开连接（幂等）
    void disconnect();

    bool is_connected() const;
    const std::string& name() const;
    /// @brief 协商后的协议版本（如 "2025-11-25" / "2026-07-28"）
    const std::string& protocol_version() const;

    // === 工具 ===
    ResultV2<std::vector<McpToolInfo>> list_tools();
    ResultV2<McpCallResult> call_tool(const std::string& tool_name,
                                      const nlohmann::json& args);

    // === 资源 ===
    ResultV2<std::vector<McpResourceInfo>> list_resources();
    ResultV2<std::vector<McpResourceContent>> read_resource(const std::string& uri);

private:
    /// @brief 发送请求并解析 result（error 转为 Error）
    ResultV2<nlohmann::json> request(const std::string& method,
                                     const nlohmann::json& params,
                                     int timeout_ms);
    /// @brief 发送请求返回完整响应（含 error，供协商逻辑检查）
    ResultV2<nlohmann::json> raw_request(const std::string& method,
                                         const nlohmann::json& params,
                                         int timeout_ms);
    /// @brief 发送通知
    ResultV2<void> notify(const std::string& method, const nlohmann::json& params);

    /// @brief 1.x initialize 握手
    ResultV2<nlohmann::json> do_initialize(const std::string& version, int timeout_ms);

    std::unique_ptr<McpTransport> m_transport;
    std::string m_name;
    std::string m_protocol_version;
    bool m_connected = false;
    bool m_stateless = false;  ///< 2.0 无状态模式（每请求 _meta 带版本）
    int m_next_id = 1;
};

} // namespace agent::mcp
