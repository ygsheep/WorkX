/**
 * @file i_file_loader.h
 * @brief 文件加载器抽象接口（H-11：副作用隔离）
 * @details 抽出 InputProcessor 中 std::ifstream 直读文件的副作用，使其可单元测试。
 *          生产用 LocalFileLoader，测试用 InMemoryFileLoader。
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <optional>
#include <string>

namespace agent::input {

/// @brief 文件加载器抽象接口
/// @details H-11：InputProcessor 依赖此接口而非直接 std::ifstream，
///          测试可注入 InMemoryFileLoader 模拟文件内容与错误场景。
class IFileLoader {
public:
    virtual ~IFileLoader() = default;

    /// @brief 读取文件内容
    /// @param path 文件路径
    /// @return 文件内容字符串；失败时返回 std::nullopt（如文件不存在、无权限）
    /// @note C-2：返回 std::optional<std::string> 以区分"失败"与"空文件"。
    ///       std::nullopt 表示失败；std::optional("") 表示合法的空文件。
    /// @note 实现应保证线程安全（生产 LocalFileLoader 通过 std::ifstream 局部变量天然线程安全）
    virtual std::optional<std::string> load(const std::string& path) = 0;
};

} // namespace agent::input

#include <fstream>
#include <iterator>
#include <system_error>

namespace agent::input {

/// @brief 本地文件系统加载器（生产实现）
/// @details std::ifstream 二进制读取整个文件。无法打开时返回 std::nullopt；
///          成功读取空文件时返回 std::optional("")（C-2：区分失败与空文件）。
class LocalFileLoader final : public IFileLoader {
public:
    std::optional<std::string> load(const std::string& path) override {
        std::ifstream file(path, std::ios::binary);
        if (!file.is_open()) {
            return std::nullopt;  // 失败：无法打开文件
        }
        return std::string(std::istreambuf_iterator<char>(file),
                           std::istreambuf_iterator<char>());
    }
};

} // namespace agent::input
