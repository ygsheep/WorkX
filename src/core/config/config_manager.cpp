/**
 * @file config_manager.cpp
 * @brief 配置管理器实现
 */

#include "core/config/config_manager.h"

#ifdef WORKX_HAS_NLOHMANN_JSON
#include <nlohmann/json.hpp>
#endif

#include <fstream>
#include <filesystem>

namespace agent {

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
#ifdef WORKX_HAS_NLOHMANN_JSON
    if (!std::filesystem::exists(path)) {
        return Result<void, std::string>::err(
            std::format("Config file not found: {}", path.string())
        );
    }

    std::ifstream file(path);
    if (!file.is_open()) {
        return Result<void, std::string>::err(
            std::format("Failed to open config file: {}", path.string())
        );
    }

    // 空文件视为有效配置（无内容），优雅降级
    if (file.peek() == std::ifstream::traits_type::eof()) {
        file.close();
        return Result<void, std::string>::ok();
    }

    try {
        nlohmann::json j;
        file >> j;
        file.close();

        for (auto& [key, value] : j.items()) {
            if (value.is_boolean()) {
                m_values[key] = value.get<bool>();
            } else if (value.is_number_integer()) {
                m_values[key] = value.get<int>();
            } else if (value.is_number()) {
                m_values[key] = value.get<double>();
            } else if (value.is_string()) {
                m_values[key] = value.get<std::string>();
            }
        }

        return Result<void, std::string>::ok();

    } catch (const nlohmann::json::parse_error& e) {
        return Result<void, std::string>::err(
            std::format("JSON parse error in {}: {}", path.string(), e.what())
        );
    } catch (const std::exception& e) {
        return Result<void, std::string>::err(
            std::format("Error reading {}: {}", path.string(), e.what())
        );
    }
#else
    (void)path;
    return Result<void, std::string>::err("JSON persistence requires nlohmann/json");
#endif
}

Result<void, std::string> ConfigManager::save_to_file(const std::filesystem::path& path) {
#ifdef WORKX_HAS_NLOHMANN_JSON
    nlohmann::json j;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& [key, value] : m_values) {
            if (std::holds_alternative<bool>(value)) {
                j[key] = std::get<bool>(value);
            } else if (std::holds_alternative<int>(value)) {
                j[key] = std::get<int>(value);
            } else if (std::holds_alternative<double>(value)) {
                j[key] = std::get<double>(value);
            } else if (std::holds_alternative<std::string>(value)) {
                j[key] = std::get<std::string>(value);
            }
        }
    }

    try {
        // 确保父目录存在
        if (path.has_parent_path()) {
            std::filesystem::create_directories(path.parent_path());
        }

        std::ofstream file(path);
        if (!file.is_open()) {
            return Result<void, std::string>::err(
                std::format("Failed to create config file: {}", path.string())
            );
        }
        file << j.dump(4);
        file.close();
        return Result<void, std::string>::ok();

    } catch (const std::exception& e) {
        return Result<void, std::string>::err(
            std::format("Error writing {}: {}", path.string(), e.what())
        );
    }
#else
    (void)path;
    return Result<void, std::string>::err("JSON persistence requires nlohmann/json");
#endif
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

} // namespace workx
