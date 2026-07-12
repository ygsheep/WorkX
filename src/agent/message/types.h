/**
 * @file events.h
 * @brief 所有事件类型定义
 * @details TUI/Session/Agent 层间通信事件
 * @version 1.0.0
 */

#pragma once

#include <string>
#include <vector>

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

/// @brief 流式完成
struct StreamDoneEvent {
    std::string session_id;
    std::string full_content;
    std::string full_reasoning;
    bool was_interrupted = false;
    int32_t prompt_tokens = 0;
    int32_t generated_tokens = 0;
    double prompt_ms = 0.0;
    double generation_ms = 0.0;
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

/// @brief 工具类型
enum class ToolType {
    ReadFile,       ///< 文件读取
    WriteFile,      ///< 文件写入
    EditFile,       ///< 文件编辑
    Execute,        ///< Shell 命令
    Search,         ///< 搜索（grep/find）
    Agent,          ///< 子代理
    Other           ///< 其他/未知
};

/// @brief Agent 调用工具
struct ToolCallEvent {
    std::string tool_name;
    std::string arguments;
    std::string call_id;
    ToolType tool_type = ToolType::Other;
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
