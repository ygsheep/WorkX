/**
 * @file events.h
 * @brief 跨层通信事件类型定义（H-10：按域拆分后的聚合头文件）
 * @details 本文件为向后兼容聚合头，实际定义已拆分到三个子文件：
 *          - system_events.h：ShutdownEvent / ModelLoadEvent / BackendStatusEvent
 *          - stream_events.h：UserInput / Interrupt / Stream* / Step* 事件
 *          - agent_events.h：AgentStep / ToolCall / ToolResult / AgentDone 事件
 *
 *          新代码建议按需直接 include 子文件，避免引入无关事件类型；
 *          旧代码可继续 include 本文件，行为不变。
 *
 *          注：ChatMessage / ToolUse / CompletionRequest / StreamChunk 等
 *          消息与 DTO 类型位于 agent/api/chat_types.h；ToolType 枚举位于
 *          agent/tool/tool_kind.h。本文件仅含事件类型。
 * @version 2.0.0
 * @date 2026-07
 */

#pragma once

#include "core/events/system_events.h"
#include "core/events/stream_events.h"
#include "core/events/agent_events.h"
