/**
 * @file config_manager.cpp
 * @brief 配置管理器实现
 *
 * 注意：load_from_file / save_to_file 需要 nlohmann/json
 * 如果不需要持久化，可以删除这两个函数的实现
 */

#include "config_manager.h"
// #include <nlohmann/json.hpp>  // 取消注释以启用持久化
#include <fstream>
#include <filesystem>

namespace mydev {

// ============================================================
// JSON 持久化辅助函数（取消注释 nlohmann/json 后可用）
// ============================================================
/*
namespace {

/// 将 dot 分隔的 key 写入嵌套 JSON（如 "backend.api_key" → j["backend"]["api_key"]）
void set_nested_json(nlohmann::json& j, const std::string& key, const ConfigValue& value) {
    nlohmann::json* current = &j;
    size_t start = 0;
    while (true) {
        size_t dot = key.find('.', start);
        if (dot == std::string::npos) {
            std::string leaf = key.substr(start);
            if (std::holds_alternative<bool>(value)) {
                (*current)[leaf] = std::get<bool>(value);
            } else if (std::holds_alternative<int>(value)) {
                (*current)[leaf] = std::get<int>(value);
            } else if (std::holds_alternative<double>(value)) {
                (*current)[leaf] = std::get<double>(value);
            } else if (std::holds_alternative<std::string>(value)) {
                (*current)[leaf] = std::get<std::string>(value);
            }
            return;
        }
        std::string section = key.substr(start, dot - start);
        if (!current->contains(section) || !(*current)[section].is_object()) {
            (*current)[section] = nlohmann::json::object();
        }
        current = &(*current)[section];
        start = dot + 1;
    }
}

/// 递归展平嵌套 JSON object 为 dot 分隔的 flat key
void flatten_json(const nlohmann::json& j, const std::string& prefix,
                  std::unordered_map<std::string, ConfigValue>& values) {
    for (auto& [key, value] : j.items()) {
        std::string full_key = prefix.empty() ? key : prefix + "." + key;
        if (value.is_boolean()) {
            values[full_key] = value.get<bool>();
        } else if (value.is_number_integer()) {
            values[full_key] = value.get<int>();
        } else if (value.is_number()) {
            values[full_key] = value.get<double>();
        } else if (value.is_string()) {
            values[full_key] = value.get<std::string>();
        } else if (value.is_object()) {
            flatten_json(value, full_key, values);
        }
        // 数组和 null 被忽略
    }
}

} // anonymous namespace
*/

bool ConfigManager::has(const std::string& key) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_values.find(key) != m_values.end();
}

void ConfigManager::remove(const std::string& key) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_values.erase(key);
}

void ConfigManager::register_meta(const std::string& key, ConfigMeta meta) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_metas[key] = std::move(meta);
}

Result<ConfigMeta, std::string> ConfigManager::get_meta(const std::string& key) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_metas.find(key);
    if (it == m_metas.end()) {
        return Result<ConfigMeta, std::string>::err(
            std::format("Config meta '{}' not found", key)
        );
    }
    return Result<ConfigMeta, std::string>::ok(it->second);
}

Result<void, std::string> ConfigManager::load_from_file(const std::filesystem::path& path) {
    // 取消下方注释以启用 JSON 持久化
    /*
    if (!std::filesystem::exists(path)) {
        return Result<void, std::string>::err("File not found");
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        return Result<void, std::string>::err("Failed to open file");
    }

    try {
        nlohmann::json j;
        file >> j;
        file.close();

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            // 递归展平嵌套 JSON（支持 {"backend": {"api_key": "..."}} 格式）
            // 同时向后兼容旧的扁平格式 {"backend.api_key": "..."}
            flatten_json(j, "", m_values);
        }

        return Result<void, std::string>::ok();

    } catch (const nlohmann::json::parse_error& e) {
        return Result<void, std::string>::err(std::format("JSON parse error: {}", e.what()));
    } catch (const std::exception& e) {
        return Result<void, std::string>::err(std::format("Error: {}", e.what()));
    }
    */
    return Result<void, std::string>::err("JSON persistence not enabled");
}

Result<void, std::string> ConfigManager::save_to_file(const std::filesystem::path& path) {
    // 取消下方注释以启用 JSON 持久化
    /*
    nlohmann::json j;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& [key, value] : m_values) {
            // 将 dot 分隔的 key 写为嵌套 JSON（"backend.api_key" → j["backend"]["api_key"]）
            set_nested_json(j, key, value);
        }
    }

    try {
        std::ofstream file(path);
        if (!file.is_open()) {
            return Result<void, std::string>::err("Failed to create file");
        }
        file << j.dump(4);
        file.close();
        return Result<void, std::string>::ok();

    } catch (const std::exception& e) {
        return Result<void, std::string>::err(std::format("Error: {}", e.what()));
    }
    */
    return Result<void, std::string>::err("JSON persistence not enabled");
}

void ConfigManager::add_change_callback(
    std::function<void(const std::string&, const ConfigValue&, const ConfigValue&)> callback) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_global_change_callbacks.push_back(std::move(callback));
}

void ConfigManager::clear_change_callbacks() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_global_change_callbacks.clear();
}

void ConfigManager::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_values.clear();
    m_metas.clear();
}

void ConfigManager::clear_for_test() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_values.clear();
    m_metas.clear();
}

std::vector<std::string> ConfigManager::get_all_keys() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<std::string> keys;
    keys.reserve(m_values.size());
    for (const auto& [key, _] : m_values) {
        keys.push_back(key);
    }
    return keys;
}

// ================ ConfigScope ================

ConfigScope::ConfigScope(const std::string& prefix)
    : m_prefix(prefix) {
    if (!m_prefix.empty() && m_prefix.back() != '.') {
        m_prefix += '.';
    }
}

ConfigScope::~ConfigScope() = default;

std::string ConfigScope::make_key(const std::string& key) const {
    return m_prefix + key;
}

} // namespace mydev
