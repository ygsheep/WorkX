/**
 * @file itool.h
 * @brief ITool 接口 — 工具抽象基类
 * @details 所有 Agent 可调用工具的统一接口：名称、描述、参数 schema、权限检查、输入验证、执行
 * @version 1.1.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include "core/utils/result.h"
#include "agent/tool/result.h"
#include "agent/tool/context.h"

namespace agent::tool {

/// @brief 权限检查结果类型
using PermissionResult = Result<void, std::string>;

/// @brief 输入验证结果类型
using ValidationResult = Result<void, std::string>;

/// @brief ITool 接口 — 工具抽象基类
///
/// 所有 Agent 可调用工具的统一接口：
/// - 名称、描述、参数 schema 声明
/// - 权限检查、输入验证、执行
/// - 使用同步返回类型，不用 cppcoro::task
class ITool {
public:
    virtual ~ITool() = default;

    /// @brief 工具名称
    /// @return 工具名称字符串
    virtual const std::string& name() const = 0;

    /// @brief 工具描述
    /// @return 工具描述字符串
    virtual const std::string& description() const = 0;

    /// @brief 工具提示词（供 LLM 理解工具用途）
    /// @return 提示词字符串
    virtual const std::string& prompt() const = 0;

    /// @brief 获取参数 JSON Schema
    /// @return JSON Schema 描述工具输入参数
    virtual nlohmann::json input_schema() const = 0;

    /// @brief 检查工具调用权限（默认允许，子类可覆盖）
    /// @param input 工具输入参数
    /// @param ctx 工具执行上下文
    /// @return 权限检查结果
    virtual PermissionResult check_permissions(
        const nlohmann::json& /*input*/,
        const ToolContext& /*ctx*/
    ) const {
        return PermissionResult::ok();
    }

    /// @brief 验证工具输入（默认通过，子类可覆盖）
    /// @param input 工具输入参数
    /// @param ctx 工具执行上下文
    /// @return 验证结果
    virtual ValidationResult validate_input(
        const nlohmann::json& /*input*/,
        const ToolContext& /*ctx*/
    ) const {
        return ValidationResult::ok();
    }

    /// @brief 执行工具
    /// @param input 工具输入参数
    /// @param ctx 工具执行上下文
    /// @return 工具执行结果
    /// @par 线程安全保证（K-1 / Phase 3）
    /// `call()` 标注为 `const`：工具实例本身无可观察副作用，可被多个线程
    /// 并行调用同一实例。需要缓存可变状态的工具用 `mutable` + mutex 保护。
    /// 跨工具共享状态通过单例（如 FileHistory / FileReadStateTracker）访问，
    /// 这些单例内部已用 mutex 保护。
    virtual ToolResult call(
        const nlohmann::json& input,
        const ToolContext& ctx
    ) const = 0;
};

} // namespace agent::tool
