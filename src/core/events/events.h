/**
 * @file events.h
 * @brief 跨层通信事件类型定义（P2 从 agent/message/types.h 迁移）
 * @details 包含 UserInputEvent / InterruptEvent / StreamTokenEvent /
 *          StreamDoneEvent / StreamErrorEvent / ToolCallEvent /
 *          ToolResultEvent / AgentStepEvent / AgentDoneEvent 等跨层事件。
 *
 *          这些事件类型由各层（TUI/Agent/Core）共享，属于核心基础设施，
 *          因此从 agent/message/ 迁移到 core/events/。
 *          agent/message/types.h 已 #include 本文件作为向后兼容 shim。
 *
 *          注：ChatMessage / ToolUse / CompletionRequest / StreamChunk 等
 *          消息与 DTO 类型位于 agent/api/chat_types.h；ToolType 枚举位于
 *          core/tool_kind.h（H-3 从 agent/tool/tool_kind.h 迁入 core 层，
 *          消除 core 反向依赖 agent 的分层越界）。本文件仅含事件类型。
 * @version 1.3.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <vector>

#include "core/tool_kind.h"  // H-3：ToolType 枚举迁至 core 层，消除反向依赖

namespace agent {

// ============================================================
// 用户事件（TUI → Session）
// ============================================================
/// @brief 用户提交的原始文本（不含尾部换行）
struct UserInputEvent {
    std::string text;
};

/// @brief 中断请求（Ctrl+C）
struct InterruptEvent {
    bool force = false;  ///< 双击 Ctrl+C 时 force=true
};

// ============================================================
// 流式事件（Backend/Agent → TUI）
// ============================================================

/// @brief 单个流式 token
struct StreamTokenEvent {
    std::string session_id;
    std::string content_delta;
    std::string reasoning_delta;
    bool is_thinking = false;
    int32_t token_count = 0;
};

/// @brief Prompt 处理进度
struct StreamProgressEvent {
    std::string session_id;
    int32_t total = 0;
    int32_t processed = 0;
};

/// @brief 流式完成（整个会话/本轮推理结束）
/// @details 语义：LLM 推理完全结束（含 ReAct 多步全部完成），
///          UI 应执行完整结束流程：token 统计、状态转 IDLE、光标复位等。
///          注意：ReAct 循环中单步 LLM 输出结束（仍有 tool_use 待执行）
///          应发布 StepDoneEvent 而非本事件，避免触发会话级结束动作。
struct StreamDoneEvent {
    std::string session_id;
    std::string full_content;
    std::string full_reasoning;
    bool was_interrupted = false;
    int32_t prompt_tokens = 0;
    int32_t generated_tokens = 0;
    // 上下文管理：Anthropic cache usage（OpenAI adapter 留 0）
    // 命中 cache 时 prompt_tokens 不含 cache 部分，需单独累加
    int32_t cache_creation_input_tokens = 0;
    int32_t cache_read_input_tokens = 0;
    double prompt_ms = 0.0;
    double generation_ms = 0.0;
};

/// @brief 单步 LLM 输出结束（P3 新增，清理 StreamDoneEvent 语义污染）
/// @details 语义：ReAct 循环中一轮 LLM 流式输出结束，但仍有 tool_use 待执行，
///          后续还会有更多 LLM 调用。UI 应执行轻量收尾（spinner 停止、
///          formatter flush），但不应触发会话级结束动作（token 统计、
///          状态转 IDLE、光标复位）。
struct StepDoneEvent {
    std::string session_id;
    std::string full_content;       ///< 本步 LLM 输出的完整正文
    std::string full_reasoning;     ///< 本步 LLM 输出的完整推理内容
    double generation_ms = 0.0;     ///< 本步生成耗时（用于思考标记显示）
};

/// @brief 推理错误
struct StreamErrorEvent {
    std::string session_id;
    std::string message;
    bool retryable = false;
};

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

// ============================================================
// 系统事件（跨切面）
// ============================================================

/// @brief 模型加载进度
struct ModelLoadEvent {
    std::string model_name;
    float progress = 0.0f;
    bool complete = false;
    std::string error;
};

/// @brief 后端连接状态
struct BackendStatusEvent {
    enum Status { Disconnected, Connecting, Connected, Error } status = Disconnected;
    std::string backend_name;
    std::string error;
};

/// @brief 应用关闭请求
struct ShutdownEvent {
    bool force = false;
};

} // namespace agent
