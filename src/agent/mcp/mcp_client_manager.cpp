/**
 * @file mcp_client_manager.cpp
 * @brief MCP 连接管理实现
 * @details 从配置加载 server 列表，逐个连接；连接成功后预取工具清单缓存。
 *          单个 server 失败不阻断整体（记录到日志，其余正常连接）。
 * @version 1.0.0
 * @date 2026-08
 */

#include "agent/mcp/mcp_client_manager.h"

#include "agent/mcp/mcp_config.h"
#include "liblogger/logger.h"

namespace agent::mcp {

void McpClientManager::load_and_connect(const std::filesystem::path& user_config_dir,
                                        const std::filesystem::path& cwd) {
    auto configs = load_mcp_configs(user_config_dir, cwd);
    if (configs.is_err()) {
        LOG_ERROR("[mcp] 加载 MCP 配置失败: {}", configs.error().message);
        return;
    }

    for (const auto& cfg : configs.value()) {
        auto client = std::make_shared<McpClient>();
        auto result = client->connect(cfg);
        if (result.is_err()) {
            LOG_WARN("[mcp] MCP server '{}' 连接失败: {}",
                     cfg.name, result.error().message);
            continue;
        }
        // 预取工具清单（失败不阻断，仅无工具列表）
        std::vector<std::string> tool_names;
        auto tools = client->list_tools();
        if (tools.is_ok()) {
            for (const auto& t : tools.value()) {
                tool_names.push_back(t.name);
            }
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_clients[cfg.name] = std::move(client);
            m_tool_names[cfg.name] = std::move(tool_names);
        }
        LOG_INFO("[mcp] MCP server '{}' 已连接（协议 {}）",
                 cfg.name, m_clients.at(cfg.name)->protocol_version());
    }
}

std::shared_ptr<McpClient> McpClientManager::get_client(const std::string& name) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_clients.find(name);
    return it != m_clients.end() ? it->second : nullptr;
}

std::vector<std::shared_ptr<McpClient>> McpClientManager::clients() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::shared_ptr<McpClient>> result;
    result.reserve(m_clients.size());
    for (const auto& [_, client] : m_clients) {
        result.push_back(client);
    }
    return result;
}

std::vector<std::string> McpClientManager::server_names() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> result;
    result.reserve(m_clients.size());
    for (const auto& [name, _] : m_clients) {
        result.push_back(name);
    }
    return result;
}

std::string McpClientManager::describe_servers() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_clients.empty()) return "";
    std::string out;
    for (const auto& [name, _] : m_clients) {
        out += "- " + name;
        auto it = m_tool_names.find(name);
        if (it != m_tool_names.end() && !it->second.empty()) {
            out += "（工具：";
            for (size_t i = 0; i < it->second.size(); ++i) {
                if (i > 0) out += ", ";
                out += it->second[i];
            }
            out += "）";
        }
        out += "\n";
    }
    return out;
}

bool McpClientManager::empty() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_clients.empty();
}

} // namespace agent::mcp
