/**
 * @file session_log.cpp
 * @brief 详情视图会话日志实现
 */

#include "tui/render/session_log.h"

namespace tui {

void SessionLog::add_thought(int32_t step, std::string reasoning, std::string content, int32_t seconds) {
    LogEntry e;
    e.type = LogEntryType::Thought;
    e.step_number = step;
    e.reasoning = std::move(reasoning);
    e.content = std::move(content);
    e.thinking_seconds = seconds;
    m_entries.push_back(std::move(e));
}

void SessionLog::add_final_answer(int32_t step, std::string reasoning, std::string content, int32_t seconds) {
    LogEntry e;
    e.type = LogEntryType::FinalAnswer;
    e.step_number = step;
    e.reasoning = std::move(reasoning);
    e.content = std::move(content);
    e.thinking_seconds = seconds;
    m_entries.push_back(std::move(e));
}

void SessionLog::add_tool_call(int32_t step, std::string tool_name, std::string arguments_json) {
    LogEntry e;
    e.type = LogEntryType::ToolCall;
    e.step_number = step;
    e.tool_name = std::move(tool_name);
    e.arguments_json = std::move(arguments_json);
    m_entries.push_back(std::move(e));
}

void SessionLog::add_tool_result(int32_t step, std::string result, bool is_error) {
    // 找到最后一个 type=ToolCall 且 result 为空的 entry，填充 result（合并 ToolCall + ToolResult 为一张卡片）
    for (auto it = m_entries.rbegin(); it != m_entries.rend(); ++it) {
        if (it->type == LogEntryType::ToolCall && it->result.empty()) {
            it->result = std::move(result);
            it->is_error = is_error;
            return;
        }
    }
    // 没找到匹配的 ToolCall，单独创建 ToolResult entry
    LogEntry e;
    e.type = LogEntryType::ToolResult;
    e.step_number = step;
    e.result = std::move(result);
    e.is_error = is_error;
    m_entries.push_back(std::move(e));
}

} // namespace tui
