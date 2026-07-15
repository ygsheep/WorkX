/**
 * @file chat_types.h
 * @brief 聊天相关数据类型
 * @details ChatMessage, CompletionRequest, StreamChunk, BackendConfig, ModelInfo
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <vector>
#include <chrono>
#include <nlohmann/json.hpp>
#include "agent/model/provider_type.h"

namespace agent {

// ============================================================
// ToolUse（Assistant 消息中的工具调用块）
// ============================================================

/// @brief 工具调用（对应 Anthropic tool_use content block）
struct ToolUse {
    std::string id;             ///< 工具调用 ID（LLM 生成，如 "toolu_xxx"）
    std::string name;           ///< 工具名称（如 "Read" / "Write"）
    nlohmann::json input;       ///< 工具输入参数（JSON 对象）
};

// ============================================================
// ChatMessage
// ============================================================

/// @brief 聊天消息
struct ChatMessage {
    enum class Role {
        System,
        User,
        Assistant,
        Tool
    };

    Role role = Role::User;
    std::string content;
    std::string reasoning_content;  ///< 思考/推理内容（如 deepseek thinking）
    std::string tool_call_id;       ///< Tool 角色时的调用 ID（对应 tool_use_id）
    std::string tool_name;          ///< Tool 角色时的工具名
    std::vector<ToolUse> tool_uses; ///< Assistant 角色时的工具调用列表

    /// @brief 便捷构造
    static ChatMessage system(const std::string& text) {
        return {Role::System, text, {}, {}, {}, {}};
    }
    static ChatMessage user(const std::string& text) {
        return {Role::User, text, {}, {}, {}, {}};
    }
    static ChatMessage assistant(const std::string& text) {
        return {Role::Assistant, text, {}, {}, {}, {}};
    }

    /// @brief 构造 tool_result 消息（Tool 角色）
    /// @param tool_use_id 对应的 tool_use ID
    /// @param tool_name 工具名
    /// @param result_content 工具执行结果文本
    static ChatMessage tool_result(const std::string& tool_use_id,
                                   const std::string& tool_name,
                                   const std::string& result_content) {
        ChatMessage msg;
        msg.role = Role::Tool;
        msg.tool_call_id = tool_use_id;
        msg.tool_name = tool_name;
        msg.content = result_content;
        return msg;
    }
};

// ============================================================
// CompletionRequest
// ============================================================

/// @brief 推理请求
struct CompletionRequest {
    std::vector<ChatMessage> messages;
    std::vector<std::string> stop_words;
    nlohmann::json tools;           ///< 工具 schema 数组（function calling），空表示无工具
    int32_t max_tokens = -1;        ///< -1 表示不限制
    float temperature = 0.8f;
    float top_p = 0.95f;
    bool stream = true;             ///< 强制流式模式

    /// @brief 是否携带工具
    [[nodiscard]] bool has_tools() const { return !tools.is_null() && tools.is_array() && !tools.empty(); }
};

// ============================================================
// StreamChunk
// ============================================================

/// @brief 流式响应的单个数据块
struct StreamChunk {
    std::string content_delta;      ///< 正文增量
    std::string reasoning_delta;    ///< 推理内容增量
    int32_t token_count = 0;        ///< 当前 token 数
    bool is_final = false;          ///< 是否为最终块

    // 以下字段仅在 is_final=true 时有效
    int32_t prompt_tokens = 0;
    int32_t generated_tokens = 0;
    double prompt_ms = 0.0;
    double generation_ms = 0.0;

    // tool_use 流式事件（Anthropic content_block_start/delta）
    bool is_tool_use_start = false;     ///< tool_use content_block 开始
    bool is_tool_use_delta = false;     ///< tool_use input JSON 增量
    std::string tool_use_id;            ///< is_tool_use_start 时填充
    std::string tool_name;              ///< is_tool_use_start 时填充
    std::string tool_input_delta;       ///< is_tool_use_delta 时填充（partial JSON）
};

// ============================================================
// BackendConfig
// ============================================================

/// @brief 后端配置
struct BackendConfig {
    enum class Type {
        Remote,
        Local
    };

    Type type = Type::Remote;
    ProviderType provider = ProviderType::OpenAI;  ///< API 协议类型

    // Remote 配置
    std::string base_url;           ///< API 基础 URL
    std::string api_key;            ///< API Key
    std::string model_name;         ///< 模型名称
    int timeout_ms = 30000;         ///< HTTP 超时（毫秒）

    // Local 配置（Phase 5）
    std::string model_path;
    int n_ctx = 4096;
    int n_gpu_layers = -1;
};

// ============================================================
// ModelInfo
// ============================================================

/// @brief 模型信息
struct ModelInfo {
    std::string name;
    std::string description;
    int32_t context_length = 0;
};

} // namespace agent
