/**
 * @file tool_call_tracker.h
 * @brief 工具调用跟踪器（P2 从 ChatRenderer 提取）
 * @details 维护工具调用嵌套层级与活跃调用上下文：
 *          - ToolCallEvent 时记录上下文并增加嵌套层级
 *          - ToolResultEvent 时取出上下文并减少嵌套层级
 *          - AgentDoneEvent 时重置嵌套层级
 *          使 ChatRenderer 不再直接管理工具调用状态。
 *
 *          线程安全：indent_level 使用 atomic（与原 ChatRenderer 设计一致），
 *          pending_tool_calls 假定单线程访问（事件回调同步发布）。
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <atomic>
#include <optional>
#include <string>
#include <unordered_map>

namespace tui {

/**
 * @brief 工具调用跟踪器
 * @details 从 ChatRenderer 提取的纯数据模型，负责工具调用嵌套层级与上下文管理。
 */
class ToolCallTracker {
public:
    /// @brief 活跃工具调用上下文（用于 ToolResultEvent 推断语言/路径）
    struct ToolCallInfo {
        std::string tool_name;
        std::string arguments;  ///< 原始 JSON 字符串，用于解析 file_path 等
    };

    /// @brief 记录工具调用，返回新的嵌套层级
    /// @param call_id 工具调用 ID
    /// @param tool_name 工具名称
    /// @param arguments 原始 JSON 参数字符串
    /// @return 调用后的嵌套层级（用于渲染缩进）
    int on_tool_call(const std::string& call_id,
                     const std::string& tool_name,
                     const std::string& arguments) {
        m_pending[call_id] = ToolCallInfo{tool_name, arguments};
        return m_indent.fetch_add(1);
    }

    /// @brief 取出工具调用上下文，返回（上下文, 新嵌套层级）
    /// @param call_id 工具调用 ID
    /// @return 调用上下文（若找不到返回 nullopt）与调用后的嵌套层级
    struct Result {
        std::optional<ToolCallInfo> info;
        int indent_level;
    };
    Result on_tool_result(const std::string& call_id) {
        int prev = m_indent.fetch_sub(1);
        int new_indent = prev > 0 ? prev - 1 : 0;
        if (prev <= 0) {
            // 防御性：不允许变为负数，回滚
            m_indent.store(0);
            new_indent = 0;
        }

        std::optional<ToolCallInfo> info;
        auto it = m_pending.find(call_id);
        if (it != m_pending.end()) {
            info = std::move(it->second);
            m_pending.erase(it);
        }
        return {std::move(info), new_indent};
    }

    /// @brief 重置嵌套层级（AgentDoneEvent 时调用）
    void reset_indent() { m_indent.store(0); }

    /// @brief 清除所有状态（新会话）
    void reset() {
        m_indent.store(0);
        m_pending.clear();
    }

    // === Getters ===
    int indent_level() const { return m_indent.load(); }
    size_t pending_count() const { return m_pending.size(); }

private:
    std::atomic<int> m_indent{0};
    std::unordered_map<std::string, ToolCallInfo> m_pending;
};

} // namespace tui
