/**
 * @file context.h
 * @brief ToolContext — 工具执行上下文
 * @details 在工具执行过程中传递的运行时信息：会话 ID、工作目录、权限模式、取消信号
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <atomic>
#include <nlohmann/json.hpp>

namespace agent::tool {

/// @brief 工具执行上下文
///
/// 在工具执行过程中传递的运行时信息：
/// - 当前会话 ID 与请求 ID
/// - 工作目录路径
/// - 权限模式
/// - 中断信号（CancellationToken）
struct ToolContext {
    std::string cwd;                        ///< 工作目录
    std::string session_id;                 ///< 会话 ID
    std::string request_id;                 ///< 请求 ID
    std::string model;                      ///< 当前模型名称
    nlohmann::json options;                 ///< 额外选项

    /// @brief 外部取消信号指针（可选）
    /// @details 2.3 修复：由调用方（ReActLoop）传入 should_cancel 的地址，
    ///          使工具能即时感知外部取消请求。nullptr 时回退到内部 cancelled_。
    ///          生命周期由调用方保证（栈变量通常在 ToolContext 之上存活）。
    const std::atomic<bool>* cancel_flag = nullptr;

    /// @brief 检查是否已取消
    /// @return 已取消返回 true
    bool is_cancelled() const {
        if (cancel_flag != nullptr) {
            return cancel_flag->load(std::memory_order_acquire);
        }
        return cancelled_.load(std::memory_order_relaxed);
    }

    /// @brief 请求取消（仅当未绑定外部 cancel_flag 时有效）
    void cancel() {
        if (cancel_flag == nullptr) {
            cancelled_.store(true, std::memory_order_relaxed);
        }
        // 绑定外部 cancel_flag 时由外部负责置位，本方法无操作
    }

private:
    std::atomic<bool> cancelled_{false};    ///< 内部取消标志（fallback）
};

} // namespace agent::tool
