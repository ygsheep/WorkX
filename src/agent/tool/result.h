/**
 * @file result.h
 * @brief ToolResult — 工具结果类型
 * @details 工具执行后的返回值封装：成功/失败状态、文本输出或结构化数据、错误信息
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <nlohmann/json.hpp>

namespace agent::tool {

/// @brief 工具结果类型
///
/// 封装工具执行后的返回值：
/// - 成功/失败状态
/// - 文本输出或结构化数据
/// - 错误信息（失败时）
struct ToolResult {
    /// @brief 结果内容类型
    enum class Type {
        Text,       ///< 文本输出
        Json,       ///< 结构化数据
        Error,      ///< 错误信息
    };

    Type type{Type::Text};                  ///< 内容类型
    std::string text;                       ///< 文本内容
    nlohmann::json data;                    ///< 结构化数据（可选）
    bool is_error{false};                   ///< 是否错误

    /// @brief 创建文本结果
    /// @param text 文本内容
    /// @return 文本结果
    static ToolResult ok(std::string text) {
        return {.type = Type::Text, .text = std::move(text), .data = {}, .is_error = false};
    }

    /// @brief 创建结构化数据结果
    /// @param data JSON 数据
    /// @return 结构化结果
    static ToolResult ok(nlohmann::json data) {
        return {.type = Type::Json, .text = {}, .data = std::move(data), .is_error = false};
    }

    /// @brief 创建错误结果
    /// @param msg 错误信息
    /// @return 错误结果
    static ToolResult error(std::string msg) {
        return {.type = Type::Error, .text = std::move(msg), .data = {}, .is_error = true};
    }

    /// @brief 检查是否成功
    /// @return 成功返回 true
    bool is_ok() const { return !is_error; }

    /// @brief 格式化为 LLM 可读的文本
    /// @return 格式化后的文本
    std::string to_string() const {
        if (is_error) {
            return "Error: " + text;
        }
        if (type == Type::Json) {
            return data.dump(2);
        }
        return text;
    }
};

} // namespace agent::tool
