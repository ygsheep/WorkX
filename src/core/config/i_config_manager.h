/**
 * @file i_config_manager.h
 * @brief 配置管理器抽象接口（C-1 DI 化）
 * @details 类型擦除的虚函数接口 + 模板包装，允许测试注入 MockConfigManager，
 *          解除对 ConfigManager 单例的硬依赖。
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <variant>

#include "core/utils/result.h"

namespace agent {

using ConfigValue = std::variant<bool, int, double, std::string>;

/// @brief 配置管理器抽象接口
/// @details 类型擦除的虚函数接口供 DI 注入；模板包装方法委托虚函数，
///          调用方可像使用 ConfigManager 一样使用 IConfigManager&。
class IConfigManager {
public:
    virtual ~IConfigManager() = default;

    // === 类型擦除的虚函数（由实现类提供）===

    [[nodiscard]] virtual bool has(const std::string& key) const = 0;

    /// @brief 获取配置值（类型擦除）
    /// @return 成功返回 ConfigValue；失败返回错误消息
    [[nodiscard]] virtual Result<ConfigValue, std::string> get_value(
        const std::string& key) const = 0;

    /// @brief 设置配置值（类型擦除）
    virtual Result<void, std::string> set_value(
        const std::string& key, ConfigValue value) = 0;

    virtual Result<void, std::string> load_from_file(
        const std::filesystem::path& path) = 0;
    virtual Result<void, std::string> save_to_file(
        const std::filesystem::path& path) = 0;

    [[nodiscard]] virtual std::vector<std::string> get_all_keys() const = 0;

    // === 模板包装（非虚，委托虚函数）===

    /// @brief 获取配置值（类型安全）
    template<typename T>
    [[nodiscard]] Result<T, std::string> get(const std::string& key) const {
        auto result = get_value(key);
        if (result.isErr()) {
            return Result<T, std::string>::err(result.error());
        }
        const auto& v = result.unwrap();
        if (!std::holds_alternative<T>(v)) {
            return Result<T, std::string>::err(
                std::format("Type mismatch for config key '{}'", key));
        }
        return Result<T, std::string>::ok(std::get<T>(v));
    }

    /// @brief 获取配置值，失败时返回默认值
    template<typename T>
    [[nodiscard]] T get_or(const std::string& key, T default_value) const {
        auto result = get<T>(key);
        return result.isOk() ? result.unwrap() : default_value;
    }

    /// @brief 设置配置值（类型安全）
    template<typename T>
    Result<void, std::string> set(const std::string& key, T value) {
        return set_value(key, ConfigValue(std::move(value)));
    }
};

} // namespace agent
