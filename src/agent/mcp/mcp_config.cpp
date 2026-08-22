/**
 * @file mcp_config.cpp
 * @brief MCP server 配置解析实现
 * @details 标准 mcp.json 格式解析：
 *          {
 *            "mcpServers": {
 *              "github": { "command": "npx", "args": [...], "env": {...} },
 *              "notion": { "type": "http", "url": "...", "headers": {...} }
 *            }
 *          }
 * @version 1.0.0
 * @date 2026-08
 */

#include "agent/mcp/mcp_config.h"

#include <fstream>
#include <nlohmann/json.hpp>

namespace agent::mcp {

namespace {

/// 解析单个 server 条目；无效条目返回 false
bool parse_one_server(const std::string& name, const nlohmann::json& obj,
                      McpServerConfig& out) {
    if (!obj.is_object()) return false;

    out.name = name;

    // stdio：command + args + env
    if (obj.contains("command") && obj.at("command").is_string()) {
        out.command = obj.at("command").get<std::string>();
        if (obj.contains("args") && obj.at("args").is_array()) {
            for (const auto& a : obj.at("args")) {
                if (a.is_string()) out.args.push_back(a.get<std::string>());
            }
        }
        if (obj.contains("env") && obj.at("env").is_object()) {
            for (auto it = obj.at("env").begin(); it != obj.at("env").end(); ++it) {
                if (it.value().is_string()) {
                    out.env[it.key()] = it.value().get<std::string>();
                }
            }
        }
        return out.valid();
    }

    // http：type=http + url + headers
    const std::string type = obj.value("type", "");
    if (type == "http" && obj.contains("url") && obj.at("url").is_string()) {
        out.url = obj.at("url").get<std::string>();
        if (obj.contains("headers") && obj.at("headers").is_object()) {
            for (auto it = obj.at("headers").begin(); it != obj.at("headers").end(); ++it) {
                if (it.value().is_string()) {
                    out.headers[it.key()] = it.value().get<std::string>();
                }
            }
        }
        return out.valid();
    }

    return false;
}

/// 从单个文件加载 server 配置（文件不存在返回空，不视为错误）
ResultV2<std::vector<McpServerConfig>> load_from_file(const std::filesystem::path& path) {
    std::vector<McpServerConfig> result;
    if (path.empty() || !std::filesystem::exists(path)) {
        return ResultV2<std::vector<McpServerConfig>>::ok(std::move(result));
    }

    std::ifstream ifs(path);
    if (!ifs.is_open()) {
        return ResultV2<std::vector<McpServerConfig>>::err(
            Error::Code::ConfigParseFailed,
            "无法打开 MCP 配置文件: " + path.string(),
            path.string());
    }

    try {
        nlohmann::json root;
        ifs >> root;
        return ResultV2<std::vector<McpServerConfig>>::ok(
            parse_mcp_config_json(root));
    } catch (const nlohmann::json::exception& e) {
        return ResultV2<std::vector<McpServerConfig>>::err(
            Error::Code::ConfigParseFailed,
            "MCP 配置文件 JSON 解析失败: " + std::string(e.what()),
            path.string());
    }
}

} // anonymous namespace

std::vector<McpServerConfig> parse_mcp_config_json(const nlohmann::json& json) {
    std::vector<McpServerConfig> result;
    if (!json.is_object() || !json.contains("mcpServers")) return result;

    const auto& servers = json.at("mcpServers");
    if (!servers.is_object()) return result;

    for (auto it = servers.begin(); it != servers.end(); ++it) {
        McpServerConfig cfg;
        if (parse_one_server(it.key(), it.value(), cfg)) {
            result.push_back(std::move(cfg));
        }
    }
    return result;
}

ResultV2<std::vector<McpServerConfig>> load_mcp_configs(
    const std::filesystem::path& user_config_dir,
    const std::filesystem::path& cwd) {
    // 用户级
    auto user_result = load_from_file(user_config_dir / "mcp.json");
    if (user_result.is_err()) return user_result;
    std::vector<McpServerConfig> merged = std::move(user_result.value());

    // 项目级（同名 server 覆盖用户级）
    auto project_result = load_from_file(cwd / ".mcp.json");
    if (project_result.is_err()) return project_result;
    for (auto& cfg : project_result.value()) {
        bool replaced = false;
        for (auto& existing : merged) {
            if (existing.name == cfg.name) {
                existing = std::move(cfg);
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            merged.push_back(std::move(cfg));
        }
    }
    return ResultV2<std::vector<McpServerConfig>>::ok(std::move(merged));
}

} // namespace agent::mcp
