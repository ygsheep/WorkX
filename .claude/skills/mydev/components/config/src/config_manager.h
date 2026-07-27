/**
 * @file config_manager.h
 * @brief 配置管理器
 * @details 类型安全、分层配置、验证回调、JSON 持久化
 * @version 1.0.0
 *
 * 使用方式：复制 config_manager.h + config_manager.cpp 到项目
 * 依赖：result.h（同目录或 include path 中）
 * 可选依赖：nlohmann/json（持久化时需要）
 */

#pragma once

#include "result.h"
#include <string>
#include <variant>
#include <unordered_map>
#include <filesystem>
#include <functional>
#include <mutex>
#include <vector>

// ============================================================
// 命名空间：按需修改
// ============================================================
namespace mydev {

/**
 * @brief 配置值类型
 */
using ConfigValue = std::variant<bool, int, double, std::string>;

/**
 * @brief 配置项元数据
 */
struct ConfigMeta {
    std::string description;        ///< 配置项描述
    ConfigValue default_value;      ///< 默认值
    bool is_required = false;       ///< 是否必需

    /** @brief 验证回调 */
    using ValidateCallback = std::function<Result<void, std::string>(const ConfigValue&)>;
    ValidateCallback validate_callback;

    /** @brief 变更回调 */
    using ChangeCallback = std::function<void(const ConfigValue&)>;
    ChangeCallback change_callback;
};

/**
 * @brief 配置管理器（单例）
 *
 * @example
 *   ConfigManager::instance().set("app.window.width", 1280);
 *   auto w = ConfigManager::instance().get_or<int>("app.window.width", 800);
 */
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

    /**
     * @brief 设置配置值
     * @tparam T 值类型
     * @param key 配置键（支持点号分隔，如 "app.window.width"）
     * @param value 配置值
     * @return 成功返回 void，验证失败返回错误
     */
    template<typename T>
    Result<void, std::string> set(const std::string& key, T value) {
        ConfigValue config_value = value;

        // 在锁外查找元数据并验证
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

        // 准备回调数据
        ConfigValue old_value;
        bool has_old_value = false;
        ConfigMeta::ChangeCallback change_callback;
        bool has_change_callback = false;
        std::vector<std::function<void()>> pending_callbacks;

        // 在锁内更新数据
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

        // 在锁外调用全局回调（防死锁）
        for (const auto& cb : pending_callbacks) { cb(); }

        // 在锁外调用变更回调（防死锁）
        if (has_change_callback) { change_callback(config_value); }

        return Result<void, std::string>::ok();
    }

    /**
     * @brief 获取配置值
     * @tparam T 值类型
     * @param key 配置键
     * @return 成功返回值，失败返回错误
     */
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

    /**
     * @brief 获取配置值（带默认值，不会失败）
     */
    template<typename T>
    [[nodiscard]] T get_or(const std::string& key, T default_value) const {
        auto result = get<T>(key);
        return result.isOk() ? result.unwrap() : default_value;
    }

    [[nodiscard]] bool has(const std::string& key) const;
    void remove(const std::string& key);

    /** @brief 注册配置元数据 */
    void register_meta(const std::string& key, ConfigMeta meta);

    /** @brief 获取配置元数据 */
    [[nodiscard]] Result<ConfigMeta, std::string> get_meta(const std::string& key) const;

    /** @brief 从文件加载配置（需要 nlohmann/json） */
    Result<void, std::string> load_from_file(const std::filesystem::path& path);

    /** @brief 保存配置到文件（需要 nlohmann/json） */
    Result<void, std::string> save_to_file(const std::filesystem::path& path);

    /** @brief 添加全局变更回调 */
    void add_change_callback(
        std::function<void(const std::string&, const ConfigValue&, const ConfigValue&)> callback
    );

    /** @brief 清除全局变更回调 */
    void clear_change_callbacks();

    /** @brief 清空所有配置 */
    void clear();

    /** @brief 清空所有配置（测试专用，不记录日志） */
    void clear_for_test();

    /** @brief 获取所有配置键 */
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

/**
 * @brief 配置作用域守卫（RAII）
 * @details 自动为配置键添加前缀，方便隔离
 */
class ConfigScope {
public:
    explicit ConfigScope(const std::string& prefix);
    ~ConfigScope();

    ConfigScope(const ConfigScope&) = delete;
    ConfigScope& operator=(const ConfigScope&) = delete;

    /** @brief 获取完整配置键 */
    [[nodiscard]] std::string make_key(const std::string& key) const;

    /** @brief 设置配置值 */
    template<typename T>
    Result<void, std::string> set(const std::string& key, T value) {
        return ConfigManager::instance().set(make_key(key), std::move(value));
    }

    /** @brief 获取配置值 */
    template<typename T>
    [[nodiscard]] Result<T, std::string> get(const std::string& key, T default_value = T{}) const {
        return ConfigManager::instance().get<T>(make_key(key), default_value);
    }

    /** @brief 获取配置值（带默认值） */
    template<typename T>
    [[nodiscard]] T get_or(const std::string& key, T default_value) const {
        return ConfigManager::instance().get_or(make_key(key), default_value);
    }

private:
    std::string m_prefix;
};

} // namespace mydev
