/**
 * @file mcp_transport.cpp
 * @brief MCP 传输层实现（stdio）
 * @details StdioMcpTransport 通过持久子进程收发 JSON-RPC 消息：
 *          - send_request：写 JSON 行 → 循环读行直到匹配 id 的响应（带总超时）
 *          - 忽略非 JSON 行（server 日志）与不匹配 id 的消息（notification/其他响应）
 * @version 1.0.0
 * @date 2026-08
 */

#include "agent/mcp/mcp_transport.h"

#include <chrono>

namespace agent::mcp {

StdioMcpTransport::StdioMcpTransport(McpServerConfig cfg)
    : m_cfg(std::move(cfg)) {}

StdioMcpTransport::~StdioMcpTransport() {
    stop();
}

ResultV2<void> StdioMcpTransport::start() {
    if (m_started) return ResultV2<void>::ok();
    auto result = m_proc.start(m_cfg.command, m_cfg.args, m_cfg.env);
    if (result.is_err()) return result;
    m_started = true;
    return ResultV2<void>::ok();
}

void StdioMcpTransport::stop() {
    if (!m_started) return;
    m_proc.stop();
    m_started = false;
}

ResultV2<nlohmann::json> StdioMcpTransport::send_request(
    const nlohmann::json& msg, int timeout_ms) {
    if (!msg.contains("id")) {
        return ResultV2<nlohmann::json>::err(Error::Code::InvalidInput,
            "JSON-RPC 请求缺少 id", "StdioMcpTransport::send_request");
    }
    const auto id = msg.at("id");

    auto write = m_proc.write_line(msg.dump());
    if (write.is_err()) return write.error();

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeout_ms);

    // 忽略非 JSON 行上限，防止 server 异常输出刷爆队列
    int ignored_lines = 0;
    constexpr int kMaxIgnoredLines = 1000;

    while (true) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return ResultV2<nlohmann::json>::err(Error::Code::NetworkTimeout,
                "MCP 请求超时 (" + std::to_string(timeout_ms) + "ms)",
                "method=" + msg.value("method", ""));
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now).count();

        auto line = m_proc.read_line(static_cast<int>(remaining));
        if (line.is_err()) return line.error();

        nlohmann::json resp;
        try {
            resp = nlohmann::json::parse(line.value());
        } catch (const nlohmann::json::exception&) {
            // 非 JSON 行（server 日志）：忽略
            if (++ignored_lines > kMaxIgnoredLines) {
                return ResultV2<nlohmann::json>::err(Error::Code::StreamError,
                    "MCP server 输出过多非 JSON 数据", "StdioMcpTransport::send_request");
            }
            continue;
        }

        // 只接受匹配 id 的响应；notification（无 id）与不匹配 id 的消息忽略
        if (resp.is_object() && resp.contains("id") && resp.at("id") == id) {
            return ResultV2<nlohmann::json>::ok(std::move(resp));
        }
    }
}

ResultV2<void> StdioMcpTransport::send_notification(const nlohmann::json& msg) {
    return m_proc.write_line(msg.dump());
}

std::unique_ptr<McpTransport> create_transport(const McpServerConfig& cfg) {
    // M3：cfg.is_http() 时返回 HttpMcpTransport
    return std::make_unique<StdioMcpTransport>(cfg);
}

} // namespace agent::mcp
