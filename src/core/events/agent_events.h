/**
 * @file agent_events.h
 * @brief Agent 编排事件类型（H-10：从 events.h 按域拆分）
 * @details Agent 推理步骤、工具调用、工具结果、Agent 编排完成等事件。
 *          订阅方按需 include 本文件，避免引入系统/流式事件。
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>

#include "core/tool_kind.h"  // C-3：直接引用 core 层规范位置，避免 core→agent 分层越界

namespace agent {

// ============================================================
// Agent 事件（Agent → TUI，未来）
// ============================================================

/// @brief Agent 推理步骤
struct AgentStepEvent {
    std::string step_id;
    int32_t step_number = 0;
    std::string description;
};

/// @brief Agent 调用工具
struct ToolCallEvent {
    std::string tool_name;
    std::string arguments;
    std::string call_id;
    agent::tool::ToolType tool_type = agent::tool::ToolType::Other;
};

/// @brief 工具返回结果
struct ToolResultEvent {
    std::string call_id;
    std::string result;
    bool is_error = false;
};

/// @brief Agent 编排完成
struct AgentDoneEvent {
    std::string final_response;
    int32_t total_steps = 0;
    int32_t total_tool_calls = 0;
    double total_duration_ms = 0.0;
};

} // namespace agent
