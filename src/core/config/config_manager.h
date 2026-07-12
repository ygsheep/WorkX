/**
 * @file config_manager.h
 * @brief 配置管理器
 * @details 类型安全、分层配置、验证回调、JSON 持久化
 * @version 1.0.0
 */

#pragma once

#include "core/utils/result.h"
#include <string>
#include <variant>
#include <unordered_map>
#include <filesystem>
#include <functional>
#include <mutex>
#include <vector>

namespace agent {

using ConfigValue = std::variant<bool, int, double, std::string>;

struct ConfigMeta {
    std::string description;
    ConfigValue default_value;
    bool is_required = false;

    using ValidateCallback = std::function<Result<void, std::string>(const ConfigValue&)>;
    ValidateCallback validate_callback;

    using ChangeCallback = std::function<void(const ConfigValue&)>;
    ChangeCallback change_callback;
};

class ConfigManager final {
public:
    static ConfigManager& instance() noexcept {
        static ConfigManager inst;
        return inst;
    }

    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;
    ConfigManager(ConfigManager&&) = delete;
    ConfigManager& operator=(ConfigManager&&) = delete;

    template<typename T>
    Result<void, std::string> set(const std::string& key, T value) {
        ConfigValue config_value = value;

        auto meta_it = m_metas.end();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            meta_it = m_metas.find(key);
        }

        if (meta_it != m_metas.end()) {
            auto& meta = meta_it->second;
            if (meta.validate_callback) {
                auto result = meta.validate_callback(config_value);
                if (result.isErr()) {
                    return Result<void, std::string>::err(
                        std::format("Validation failed for '{}': {}", key, result.error())
                    );
                }
            }
        }

        ConfigValue old_value;
        bool has_old_value = false;
        ConfigMeta::ChangeCallback change_callback;
        bool has_change_callback = false;
        std::vector<std::function<void()>> pending_callbacks;

        {
            std::lock_guard<std::mutex> lock(m_mutex);

            auto old_value_it = m_values.find(key);
            has_old_value = (old_value_it != m_values.end());
            old_value = has_old_value ? old_value_it->second : config_value;

            m_values[key] = config_value;

            if (meta_it != m_metas.end() && meta_it->second.change_callback) {
                has_change_callback = true;
                change_callback = meta_it->second.change_callback;
            }

            for (const auto& callback : m_global_change_callbacks) {
                pending_callbacks.push_back([callback, key, old_value, config_value]() {
                    callback(key, old_value, config_value);
                });
            }
        }

        for (const auto& cb : pending_callbacks) { cb(); }
        if (has_change_callback) { change_callback(config_value); }

        return Result<void, std::string>::ok();
    }

    template<typename T>
    [[nodiscard]] Result<T, std::string> get(const std::string& key) const {
        std::lock_guard<std::mutex> lock(m_mutex);

        auto it = m_values.find(key);
        if (it == m_values.end()) {
            auto meta_it = m_metas.find(key);
            if (meta_it != m_metas.end()) {
                if (std::holds_alternative<T>(meta_it->second.default_value)) {
                    return Result<T, std::string>::ok(std::get<T>(meta_it->second.default_value));
                }
            }
            return Result<T, std::string>::err(std::format("Config key '{}' not found", key));
        }

        if (std::holds_alternative<T>(it->second)) {
            return Result<T, std::string>::ok(std::get<T>(it->second));
        }

        return Result<T, std::string>::err(std::format("Type mismatch for config key '{}'", key));
    }

    template<typename T>
    [[nodiscard]] T get_or(const std::string& key, T default_value) const {
        auto result = get<T>(key);
        return result.isOk() ? result.unwrap() : default_value;
    }

    [[nodiscard]] bool has(const std::string& key) const;
    void remove(const std::string& key);
    void register_meta(const std::string& key, ConfigMeta meta);
    [[nodiscard]] Result<ConfigMeta, std::string> get_meta(const std::string& key) const;

    Result<void, std::string> load_from_file(const std::filesystem::path& path);
    Result<void, std::string> save_to_file(const std::filesystem::path& path);

    void add_change_callback(
        std::function<void(const std::string&, const ConfigValue&, const ConfigValue&)> callback
    );
    void clear_change_callbacks();
    void clear();
    void clear_for_test();
    [[nodiscard]] std::vector<std::string> get_all_keys() const;

private:
    ConfigManager() = default;
    ~ConfigManager() = default;

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, ConfigValue> m_values;
    std::unordered_map<std::string, ConfigMeta> m_metas;
    std::vector<std::function<void(const std::string&, const ConfigValue&, const ConfigValue&)>>
        m_global_change_callbacks;
};

class ConfigScope {
public:
    explicit ConfigScope(const std::string& prefix);
    ~ConfigScope();

    ConfigScope(const ConfigScope&) = delete;
    ConfigScope& operator=(const ConfigScope&) = delete;

    [[nodiscard]] std::string make_key(const std::string& key) const;

    template<typename T>
    Result<void, std::string> set(const std::string& key, T value) {
        return ConfigManager::instance().set(make_key(key), std::move(value));
    }

    template<typename T>
    [[nodiscard]] Result<T, std::string> get(const std::string& key, T default_value = T{}) const {
        return ConfigManager::instance().get<T>(make_key(key), default_value);
    }

    template<typename T>
    [[nodiscard]] T get_or(const std::string& key, T default_value) const {
        return ConfigManager::instance().get_or(make_key(key), default_value);
    }

private:
    std::string m_prefix;
};

} // namespace workx
