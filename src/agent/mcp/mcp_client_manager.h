/**
 * @file mcp_client_manager.h
 * @brief MCP 连接管理（Issue #27）
 * @details 管理所有已连接 MCP server 的 client：
 *          - 从配置加载并连接（会话启动时调用，失败不阻断）
 *          - 按 server 名获取 client
 *          - 缓存工具清单快照，供 MCPTool prompt 注入
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "agent/mcp/mcp_client.h"
#include "agent/mcp/mcp_types.h"

namespace agent::mcp {

/// @brief MCP 连接管理器（生命周期 = 会话）
class McpClientManager {
public:
    /// @brief 从配置加载并连接所有 server
    /// @param user_config_dir 用户配置目录（读取 <dir>/mcp.json）
    /// @param cwd 项目目录（读取 <cwd>/.mcp.json，项目级优先合并）
    /// @details 单个 server 连接失败仅记录，不阻断整体；连接成功后预取工具清单
    void load_and_connect(const std::filesystem::path& user_config_dir,
                          const std::filesystem::path& cwd);

    /// @brief 按 server 名获取 client（不存在返回 nullptr）
    std::shared_ptr<McpClient> get_client(const std::string& name) const;

    /// @brief 所有已连接 client
    std::vector<std::shared_ptr<McpClient>> clients() const;

    /// @brief 已连接 server 名列表
    std::vector<std::string> server_names() const;

    /// @brief 已连接 server 的工具清单快照（供 MCPTool prompt 注入）
    /// @details 形如 "- github（工具：create_issue, list_pulls）\n- notion（...）"
    std::string describe_servers() const;

    /// @brief 是否有任何已连接 server
    bool empty() const;

private:
    mutable std::mutex m_mutex;
    std::map<std::string, std::shared_ptr<McpClient>> m_clients;
    /// server 名 → 工具名列表（连接时预取缓存）
    std::map<std::string, std::vector<std::string>> m_tool_names;
};

} // namespace agent::mcp
