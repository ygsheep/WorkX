/**
 * @file executor.h
 * @brief ToolExecutor — 工具执行器
 * @details 负责查找工具、权限检查、输入验证、执行工具并返回结果
 * @version 1.1.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <memory>
#include <filesystem>
#include <nlohmann/json.hpp>
#include "liblogger/logger.h"
#include "itool.h"
#include "registry.h"
#include "result.h"
#include "context.h"

namespace agent::tool {

/// @brief 工具执行结果
struct ExecutionResult {
    std::string tool_name;                  ///< 工具名称
    ToolResult result;                      ///< 工具返回结果
    bool is_error{false};                   ///< 是否出错
};

/// @brief ToolExecutor — 工具执行器
///
/// 负责实际执行 LLM 请求的工具调用：
/// - 按名称查找工具
/// - 权限检查、输入验证
/// - 调用工具并返回结果
/// - 使用同步返回类型，不用 cppcoro::task
class ToolExecutor {
public:
    /// @brief 构造函数
    /// @param registry 工具注册表
    explicit ToolExecutor(std::shared_ptr<ToolRegistry> registry)
        : registry_(std::move(registry)) {}

    /// @brief 执行工具
    /// @param tool_name 工具名称
    /// @param input 工具输入参数
    /// @param ctx 工具执行上下文
    /// @return 执行结果
    inline ExecutionResult execute(
        const std::string& tool_name,
        const nlohmann::json& input,
        const ToolContext& ctx
    ) const {
        ExecutionResult exec_result;
        exec_result.tool_name = tool_name;

        LOG_DEBUG("[tool_executor] begin, tool={}, input_size={}",
                  tool_name, input.dump().size());

        // 1. 查找工具
        auto tool = registry_->find_by_name(tool_name);
        if (!tool) {
            LOG_WARN("[tool_executor] tool not found: {}", tool_name);
            exec_result.result = ToolResult::error("Tool not found: " + tool_name);
            exec_result.is_error = true;
            return exec_result;
        }

        // 2. 检查取消
        if (ctx.is_cancelled()) {
            LOG_INFO("[tool_executor] tool={} cancelled before execution", tool_name);
            exec_result.result = ToolResult::error("Tool execution cancelled");
            exec_result.is_error = true;
            return exec_result;
        }

        // 3. 权限检查
        auto perm = tool->check_permissions(input, ctx);
        if (perm.isErr()) {
            LOG_WARN("[tool_executor] tool={} permission denied: {}",
                     tool_name, perm.error());
            exec_result.result = ToolResult::error("Permission denied: " + perm.error());
            exec_result.is_error = true;
            return exec_result;
        }

        // 4. 输入验证
        auto validation = tool->validate_input(input, ctx);
        if (validation.isErr()) {
            LOG_WARN("[tool_executor] tool={} invalid input: {}",
                     tool_name, validation.error());
            exec_result.result = ToolResult::error("Invalid input: " + validation.error());
            exec_result.is_error = true;
            return exec_result;
        }

        // 5. 执行工具（try-catch 防止异常逃逸致 Agent 崩溃）
        auto t0 = std::chrono::steady_clock::now();
        try {
            exec_result.result = tool->call(input, ctx);
            exec_result.is_error = exec_result.result.is_error;
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            LOG_INFO("[tool_executor] tool={} end, is_error={}, duration={}ms",
                     tool_name, exec_result.is_error, ms);
        } catch (const nlohmann::json::exception& e) {
            LOG_ERROR("[tool_executor] tool={} JSON exception: {}",
                      tool_name, e.what());
            exec_result.result = ToolResult::error(
                std::string{"JSON error in tool '"} + tool_name + "': " + e.what());
            exec_result.is_error = true;
        } catch (const std::filesystem::filesystem_error& e) {
            LOG_ERROR("[tool_executor] tool={} filesystem exception: {}",
                      tool_name, e.what());
            exec_result.result = ToolResult::error(
                std::string{"Filesystem error in tool '"} + tool_name + "': " + e.what());
            exec_result.is_error = true;
        } catch (const std::bad_alloc& e) {
            LOG_ERROR("[tool_executor] tool={} bad_alloc: {}", tool_name, e.what());
            exec_result.result = ToolResult::error(
                std::string{"Out of memory in tool '"} + tool_name + "': " + e.what());
            exec_result.is_error = true;
        } catch (const std::exception& e) {
            LOG_ERROR("[tool_executor] tool={} std::exception: {}",
                      tool_name, e.what());
            exec_result.result = ToolResult::error(
                std::string{"Error in tool '"} + tool_name + "': " + e.what());
            exec_result.is_error = true;
        } catch (...) {
            LOG_ERROR("[tool_executor] tool={} unknown exception", tool_name);
            exec_result.result = ToolResult::error(
                std::string{"Unknown exception in tool '"} + tool_name + "'");
            exec_result.is_error = true;
        }
        return exec_result;
    }

private:
    std::shared_ptr<ToolRegistry> registry_;
};

} // namespace agent::tool
