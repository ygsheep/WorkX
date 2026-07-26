/**
 * @file config_manager.h
 * @brief 配置管理器
 * @details 类型安全、分层配置、验证回调、JSON 持久化。
 *          继承 IConfigManager 支持 DI 注入（C-1）。
 * @version 2.0.0
 */

#pragma once

#include "core/utils/result.h"
#include "core/config/i_config_manager.h"
#include <string>
#include <variant>
#include <unordered_map>
#include <filesystem>
#include <functional>
#include <mutex>
#include <vector>

namespace agent {

struct ConfigMeta {
    std::string description;
    ConfigValue default_value;
    bool is_required = false;

    using ValidateCallback = std::function<Result<void, std::string>(const ConfigValue&)>;
    ValidateCallback validate_callback;

    using ChangeCallback = std::function<void(const ConfigValue&)>;
    ChangeCallback change_callback;
};

class ConfigManager final : public IConfigManager {
public:
    static ConfigManager& instance() noexcept {
        static ConfigManager inst;
        return inst;
    }

    ConfigManager(const ConfigManager&) = delete;
    ConfigManager& operator=(const ConfigManager&) = delete;
    ConfigManager(ConfigManager&&) = delete;
    ConfigManager& operator=(ConfigManager&&) = delete;

    // === IConfigManager 类型擦除接口实现 ===

    [[nodiscard]] bool has(const std::string& key) const override;

    [[nodiscard]] Result<ConfigValue, std::string> get_value(
        const std::string& key) const override;

    Result<void, std::string> set_value(
        const std::string& key, ConfigValue value) override;

    Result<void, std::string> load_from_file(
        const std::filesystem::path& path) override;
    Result<void, std::string> save_to_file(
        const std::filesystem::path& path) override;

    [[nodiscard]] std::vector<std::string> get_all_keys() const override;

    // === 保留的 template 方法（与基类模板包装功能相同，供已存在调用方使用）===
    // 注意：基类 IConfigManager 已提供 set/get/get_or 模板包装，这里不再重复声明，
    //       调用方通过 IConfigManager& 接口即可使用。下方 register_meta 等
    //       非模板扩展方法仍保留为 ConfigManager 专属。

    void remove(const std::string& key);
    void register_meta(const std::string& key, ConfigMeta meta);
    [[nodiscard]] Result<ConfigMeta, std::string> get_meta(const std::string& key) const;

    void add_change_callback(
        std::function<void(const std::string&, const ConfigValue&, const ConfigValue&)> callback
    );
    void clear_change_callbacks();
    void clear();
    void clear_for_test();

private:
    ConfigManager() = default;
    ~ConfigManager() override = default;

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
    [[nodiscard]] Result<T, std::string> get(const std::string& key) const {
        return ConfigManager::instance().get<T>(make_key(key));
    }

    // F.4：原 get(key, default_value) 调用 ConfigManager::get<T>(key, default_value)，
    // 但 ConfigManager 只有单参数 get<T>(key) 和 get_or<T>(key, default)。
    // 修正：提供独立的 get_or 委托（已在下方），get 不再带默认值参数避免编译错误。
    // 若调用方需要默认值，应使用 get_or。

    template<typename T>
    [[nodiscard]] T get_or(const std::string& key, T default_value) const {
        return ConfigManager::instance().get_or(make_key(key), default_value);
    }

private:
    std::string m_prefix;
};

} // namespace agent
