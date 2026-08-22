/**
 * @file mcp_transport.h
 * @brief MCP 传输层抽象（Issue #27）
 * @details 传输层负责 JSON-RPC 消息的收发：
 *          - StdioMcpTransport：spawn 持久子进程，stdin 写 / stdout 行读
 *          - HttpMcpTransport：Streamable HTTP（M3 实现）
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "agent/mcp/mcp_config.h"
#include "agent/mcp/mcp_stdio_process.h"
#include "core/utils/result_v2.h"

namespace agent::mcp {

/// @brief 传输抽象：发送 JSON-RPC 消息并同步等待响应
class McpTransport {
public:
    virtual ~McpTransport() = default;

    /// @brief 启动传输（spawn 子进程 / 建立连接）
    virtual ResultV2<void> start() = 0;
    /// @brief 停止传输（终止子进程 / 关闭连接，幂等）
    virtual void stop() = 0;

    /// @brief 发送请求（单条 JSON-RPC 消息），同步阻塞等待匹配 id 的响应
    /// @param msg JSON-RPC 请求（含 id）
    /// @param timeout_ms 总超时（含等待响应）
    /// @return ok: 完整 JSON-RPC 响应（含 result 或 error）；err: 传输错误/超时
    virtual ResultV2<nlohmann::json> send_request(
        const nlohmann::json& msg, int timeout_ms) = 0;

    /// @brief 发送通知（无需响应）
    virtual ResultV2<void> send_notification(const nlohmann::json& msg) = 0;
};

/// @brief stdio 传输：持久子进程 + 双向管道
class StdioMcpTransport : public McpTransport {
public:
    explicit StdioMcpTransport(McpServerConfig cfg);
    ~StdioMcpTransport() override;

    ResultV2<void> start() override;
    void stop() override;
    ResultV2<nlohmann::json> send_request(
        const nlohmann::json& msg, int timeout_ms) override;
    ResultV2<void> send_notification(const nlohmann::json& msg) override;

private:
    McpServerConfig m_cfg;
    McpStdioProcess m_proc;
    bool m_started = false;
};

/// @brief 创建传输实例（按配置选择 stdio/http）
/// @param cfg server 配置
/// @return 传输实例（http 传输在 M3 实现，当前仅 stdio）
std::unique_ptr<McpTransport> create_transport(const McpServerConfig& cfg);

} // namespace agent::mcp
