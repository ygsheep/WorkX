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
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <unordered_map>
#include <filesystem>
#include <functional>
#include <mutex>
#include <vector>

namespace agent {

/// @brief 配置元数据（旧版，向后兼容）
struct ConfigMeta {
    std::string description;
    ConfigValue default_value;
    bool is_required = false;

    using ValidateCallback = std::function<Result<void, std::string>(const ConfigValue&)>;
    ValidateCallback validate_callback;

    using ChangeCallback = std::function<void(const ConfigValue&)>;
    ChangeCallback change_callback;
};

/// @brief 结构化配置 Schema（C-2/C-4）
/// @details 在 ConfigMeta 基础上增加类型约束、范围约束、枚举值、环境变量映射。
///          register_schema() 注册后，set_value() 时自动校验。
struct ConfigSchema {
    std::string key;                   ///< 配置键
    std::string description;           ///< 人类可读描述
    ConfigValue default_value;         ///< 默认值
    bool is_required = false;          ///< 是否必填

    /// @brief 类型约束
    enum class Type : int {
        Bool,   ///< 布尔
        Int,    ///< 整数
        Double, ///< 浮点
        String, ///< 字符串
        Enum    ///< 枚举（enum_values 限定）
    } type = Type::String;

    /// @brief 整数范围约束（仅 type=Int 时生效）
    std::optional<std::pair<int64_t, int64_t>> int_range;
    /// @brief 浮点范围约束（仅 type=Double 时生效）
    std::optional<std::pair<double, double>> double_range;
    /// @brief 枚举值列表（仅 type=Enum 时生效）
    std::vector<std::string> enum_values;

    /// @brief 对应的环境变量名（C-4，空表示不绑定环境变量）
    std::string env_var;

    /// @brief 自定义验证函数（与类型/范围/枚举校验叠加，均需通过）
    std::function<Result<void, std::string>(const ConfigValue&)> validate;

    /// @brief 校验配置值是否符合 schema
    /// @param value 待校验的值
    /// @return 成功返回 ok；失败返回错误消息
    [[nodiscard]] Result<void, std::string> validate_value(const ConfigValue& value) const;
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

    // === C-2/C-4：结构化 Schema ===

    /// @brief 注册配置 Schema
    /// @details 注册后 set_value() 时自动校验类型/范围/枚举/自定义验证
    void register_schema(ConfigSchema schema);

    /// @brief 获取配置 Schema
    [[nodiscard]] Result<ConfigSchema, std::string> get_schema(const std::string& key) const;

    /// @brief 获取所有已注册 Schema
    [[nodiscard]] std::vector<ConfigSchema> get_all_schemas() const;

    /// @brief 从环境变量加载配置（C-4）
    /// @details 遍历所有已注册 Schema，若 env_var 非空且对应环境变量存在，
    ///          按 schema.type 解析值并 set_value。默认值也会被应用。
    void load_from_env();

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
    std::unordered_map<std::string, ConfigSchema> m_schemas;  ///< C-2 结构化 Schema
    std::vector<std::function<void(const std::string&, const ConfigValue&, const ConfigValue&)>>
        m_global_change_callbacks;
};

class ConfigScope {
public:
    /// @brief 构造 ConfigScope
    /// @param prefix 键前缀（如 "backend."）
    /// @param cm 配置管理器引用（C-3 DI 化，默认使用 ConfigManager::instance()）
    explicit ConfigScope(const std::string& prefix,
                         IConfigManager& cm = ConfigManager::instance());
    ~ConfigScope();

    ConfigScope(const ConfigScope&) = delete;
    ConfigScope& operator=(const ConfigScope&) = delete;

    [[nodiscard]] std::string make_key(const std::string& key) const;

    /// @brief 获取底层配置管理器引用（供需要直接访问的场景使用）
    [[nodiscard]] IConfigManager& config_manager() const { return m_config.get(); }

    template<typename T>
    Result<void, std::string> set(const std::string& key, T value) {
        return m_config.get().set(make_key(key), std::move(value));
    }

    template<typename T>
    [[nodiscard]] Result<T, std::string> get(const std::string& key) const {
        return m_config.get().get<T>(make_key(key));
    }

    template<typename T>
    [[nodiscard]] T get_or(const std::string& key, T default_value) const {
        return m_config.get().get_or(make_key(key), default_value);
    }

private:
    std::string m_prefix;
    std::reference_wrapper<IConfigManager> m_config;
};

} // namespace agent
