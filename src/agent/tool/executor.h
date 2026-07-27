/**
 * @file executor.h
 * @brief ToolExecutor — 工具执行器
 * @details 负责查找工具、权限检查、输入验证、执行工具并返回结果
 *          V2-4：execute() 返回 ResultV2<ExecutionResult>，错误携带 Error
 *          M-3：execute() 拆分为 lookup_tool / run_with_safety / finalize_result
 *               三个职责单一的私有方法，便于独立测试与未来替换日志策略
 * @version 2.1.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <memory>
#include <chrono>
#include <filesystem>
#include <nlohmann/json.hpp>
#include "liblogger/logger.h"
#include "core/utils/result_v2.h"
#include "core/utils/error.h"
#include "itool.h"
#include "registry.h"
#include "result.h"
#include "context.h"

namespace agent::tool {

/// @brief 工具结果最大长度（超出时截断保留头尾）
/// @details 3.4：防止 grep 大仓库 / bash 长日志撑爆上下文。
///          截断策略：保留头部和尾部（通常包含错误信息和总结），省略中间。
constexpr size_t MAX_TOOL_RESULT_LENGTH = 8000;

/// @brief 工具执行结果（V2：成功载荷）
/// @details
/// V2-4 迁移：移除 is_error 字段，错误状态由 ResultV2<ExecutionResult> 承载。
/// 字段语义：
/// - `tool_name`：上下文信息，标识本次执行调用的工具（用于日志/UI 展示）
/// - `result`：工具实际返回的成功结果（ToolResult）
/// - `was_truncated`：结果元信息，标记 `result.text` 是否被截断过
struct ExecutionResult {
    std::string tool_name;                  ///< 上下文：工具名称
    ToolResult result;                      ///< 工具返回结果（成功载荷）
    bool was_truncated{false};              ///< 元信息：result.text 是否被截断

    /// @brief 结果是否被截断
    bool is_truncated() const noexcept { return was_truncated; }

    /// @brief 转换为 LLM 可读的文本（委托给 ToolResult::to_string）
    std::string to_string() const { return result.to_string(); }
};

/// @brief 截断工具结果文本（保留头尾，省略中间）
/// @param text 待截断文本（in-out）
/// @param max_length 最大保留长度
/// @return 是否发生了截断
inline bool truncate_result(std::string& text, size_t max_length = MAX_TOOL_RESULT_LENGTH) {
    if (text.length() <= max_length) return false;
    const size_t half = max_length / 2;
    const size_t omitted = text.length() - max_length;
    text = text.substr(0, half)
         + "\n\n... [output truncated, "
         + std::to_string(omitted)
         + " characters omitted] ...\n\n"
         + text.substr(text.length() - half);
    return true;
}

/// @brief 工具执行器内部使用的执行轨迹（M-3）
/// @details run_with_safety 填充，finalize_result 消费。
///          将"执行阶段"与"日志/截断阶段"解耦，便于未来注入 IToolExecutorObserver
///          或返回 ExecutionTrace 让调用方决定是否记录日志。
struct ExecutionTrace {
    std::string tool_name;
    std::chrono::steady_clock::time_point start_time;
    std::chrono::milliseconds duration{0};
    bool ok{false};
};

/// @brief ToolExecutor — 工具执行器
///
/// 负责实际执行 LLM 请求的工具调用：
/// - 按名称查找工具
/// - 权限检查、输入验证
/// - 调用工具并返回结果
/// - 使用同步返回类型，不用 cppcoro::task
///
/// M-3：execute() 内部委托给三个职责单一的私有方法：
///   - lookup_tool()：纯查找，无副作用
///   - run_with_safety()：try-catch 包装工具调用，返回 ResultV2<ToolResult>
///   - finalize_result()：组装 ExecutionResult + 截断 + 日志
class ToolExecutor {
public:
    /// @brief 构造函数
    /// @param registry 工具注册表
    explicit ToolExecutor(std::shared_ptr<ToolRegistry> registry)
        : registry_(std::move(registry)) {}

    /// @brief 执行工具（M-3：内部委托 lookup_tool / run_with_safety / finalize_result）
    /// @param tool_name 工具名称
    /// @param input 工具输入参数
    /// @param ctx 工具执行上下文
    /// @return 执行结果（V2：ResultV2<ExecutionResult>，错误携带 Error）
    inline ResultV2<ExecutionResult> execute(
        const std::string& tool_name,
        const nlohmann::json& input,
        const ToolContext& ctx
    ) const {
        LOG_DEBUG("[tool_executor] begin, tool={}, input_size={}",
                  tool_name, input.dump().size());

        // 1. 查找工具
        auto tool = lookup_tool(tool_name);
        if (!tool) {
            return Error{Error::Code::ResourceNotFound,
                         "Tool not found: " + tool_name,
                         tool_name};
        }

        // 2. 检查取消信号
        if (ctx.is_cancelled()) {
            LOG_INFO("[tool_executor] tool={} cancelled before execution", tool_name);
            return Error{Error::Code::Cancelled,
                         "Tool execution cancelled",
                         tool_name};
        }

        // 3. 权限检查
        auto perm = tool->check_permissions(input, ctx);
        if (perm.is_err()) {
            LOG_WARN("[tool_executor] tool={} permission denied: {}",
                     tool_name, perm.error().message);
            return perm.error();
        }

        // 4. 输入验证
        auto validation = tool->validate_input(input, ctx);
        if (validation.is_err()) {
            LOG_WARN("[tool_executor] tool={} invalid input: {}",
                     tool_name, validation.error().message);
            return validation.error();
        }

        // 5. 执行工具（try-catch 包装），返回 ToolResult 或 Error
        auto call_result = run_with_safety(*tool, tool_name, input, ctx);
        if (call_result.is_err()) {
            return call_result.error();
        }

        // 6. 组装结果 + 截断 + 日志
        return finalize_result(tool_name, std::move(call_result).value());
    }

private:
    std::shared_ptr<ToolRegistry> registry_;

    /// @brief 查找工具（纯查找，无副作用）
    /// @param tool_name 工具名称
    /// @return 工具实例（未找到返回 nullptr）
    inline std::shared_ptr<ITool> lookup_tool(const std::string& tool_name) const {
        auto tool = registry_->find_by_name(tool_name);
        if (!tool) {
            LOG_WARN("[tool_executor] tool not found: {}", tool_name);
        }
        return tool;
    }

    /// @brief 在 try-catch 包装下执行工具调用
    /// @details 捕获 json/filesystem/bad_alloc/std::exception/未知异常，统一转为 Error
    /// @return 成功返回 ToolResult；失败返回 Error
    inline ResultV2<ToolResult> run_with_safety(
        ITool& tool,
        const std::string& tool_name,
        const nlohmann::json& input,
        const ToolContext& ctx
    ) const {
        auto t0 = std::chrono::steady_clock::now();
        try {
            auto call_result = tool.call(input, ctx);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            if (call_result.is_err()) {
                LOG_INFO("[tool_executor] tool={} end, error, duration={}ms",
                         tool_name, ms);
                return call_result.error();
            }
            LOG_INFO("[tool_executor] tool={} end, ok, duration={}ms",
                     tool_name, ms);
            return call_result;
        } catch (const nlohmann::json::exception& e) {
            LOG_ERROR("[tool_executor] tool={} JSON exception: {}",
                      tool_name, e.what());
            return Error{Error::Code::ToolExecutionFailed,
                         std::string{"JSON error in tool '"} + tool_name + "': " + e.what(),
                         tool_name};
        } catch (const std::filesystem::filesystem_error& e) {
            LOG_ERROR("[tool_executor] tool={} filesystem exception: {}",
                      tool_name, e.what());
            return Error{Error::Code::ToolExecutionFailed,
                         std::string{"Filesystem error in tool '"} + tool_name + "': " + e.what(),
                         tool_name};
        } catch (const std::bad_alloc& e) {
            LOG_ERROR("[tool_executor] tool={} bad_alloc: {}", tool_name, e.what());
            return Error{Error::Code::InternalError,
                         std::string{"Out of memory in tool '"} + tool_name + "': " + e.what(),
                         tool_name};
        } catch (const std::exception& e) {
            LOG_ERROR("[tool_executor] tool={} std::exception: {}",
                      tool_name, e.what());
            return Error{Error::Code::ToolExecutionFailed,
                         std::string{"Error in tool '"} + tool_name + "': " + e.what(),
                         tool_name};
        } catch (...) {
            LOG_ERROR("[tool_executor] tool={} unknown exception", tool_name);
            return Error{Error::Code::Unknown,
                         std::string{"Unknown exception in tool '"} + tool_name + "'",
                         tool_name};
        }
    }

    /// @brief 组装 ExecutionResult 并执行截断
    /// @param tool_name 工具名称
    /// @param result 工具返回的 ToolResult
    /// @return 最终的 ExecutionResult
    inline ResultV2<ExecutionResult> finalize_result(
        const std::string& tool_name,
        ToolResult result
    ) const {
        ExecutionResult exec_result;
        exec_result.tool_name = tool_name;
        exec_result.result = std::move(result);

        // 3.4：结果截断（防止 grep/bash 长输出撑爆上下文）
        if (!exec_result.result.text.empty() &&
            exec_result.result.text.length() > MAX_TOOL_RESULT_LENGTH) {
            exec_result.was_truncated = truncate_result(exec_result.result.text);
            LOG_INFO("[tool_executor] tool={} result truncated, new_len={}",
                     tool_name, exec_result.result.text.length());
        }
        return ResultV2<ExecutionResult>::ok(std::move(exec_result));
    }
};

} // namespace agent::tool
