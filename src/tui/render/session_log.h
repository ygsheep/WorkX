/**
 * @file session_log.h
 * @brief 详情视图会话日志
 * @details 按时间顺序累积一轮 Agent 编排的所有条目（Thought/ToolCall/ToolResult/FinalAnswer），
 *          供 Ctrl+O 详情视图渲染。每轮用户输入开始时清空。
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace tui {

/// @brief 日志条目类型
enum class LogEntryType {
    Thought,        ///< Thought 阶段（含中间轮 + 最终答案轮）
    ToolCall,       ///< 工具调用
    ToolResult,     ///< 工具返回
    FinalAnswer,    ///< 最终答案（无 tool_use 的 Thought）
};

/// @brief 单条日志条目
struct LogEntry {
    LogEntryType type = LogEntryType::Thought;
    int32_t step_number = 0;             ///< ReAct 步骤号

    // Thought / FinalAnswer
    std::string reasoning;               ///< 推理内容
    std::string content;                 ///< 正文内容
    int32_t thinking_seconds = 0;        ///< 思考持续秒数

    // ToolCall
    std::string tool_name;               ///< 工具名（Read/Write/...）
    std::string arguments_json;          ///< 工具参数（JSON 字符串）

    // ToolResult
    std::string result;                  ///< 工具返回内容
    bool is_error = false;
};

/// @brief 会话日志（单轮 Agent 编排）
class SessionLog {
public:
    /// @brief 清空（新一轮 Agent 编排开始时调用）
    void clear() { m_entries.clear(); }

    /// @brief 追加 Thought 步骤
    void add_thought(int32_t step, std::string reasoning, std::string content, int32_t seconds);

    /// @brief 追加 FinalAnswer 步骤
    void add_final_answer(int32_t step, std::string reasoning, std::string content, int32_t seconds);

    /// @brief 追加 ToolCall 步骤
    void add_tool_call(int32_t step, std::string tool_name, std::string arguments_json);

    /// @brief 追加 ToolResult 步骤
    void add_tool_result(int32_t step, std::string result, bool is_error);

    /// @brief 获取所有条目
    const std::vector<LogEntry>& entries() const { return m_entries; }

    /// @brief 是否为空
    bool empty() const { return m_entries.empty(); }

    /// @brief 条目数
    size_t size() const { return m_entries.size(); }

private:
    std::vector<LogEntry> m_entries;
};

} // namespace tui
