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
#include "agent/model/provider_type.h"

namespace workx {

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
    std::string tool_call_id;       ///< Tool 角色时的调用 ID
    std::string tool_name;          ///< Tool 角色时的工具名

    /// @brief 便捷构造
    static ChatMessage system(const std::string& text) {
        return {Role::System, text, {}, {}, {}};
    }
    static ChatMessage user(const std::string& text) {
        return {Role::User, text, {}, {}, {}};
    }
    static ChatMessage assistant(const std::string& text) {
        return {Role::Assistant, text, {}, {}, {}};
    }
};

// ============================================================
// CompletionRequest
// ============================================================

/// @brief 推理请求
struct CompletionRequest {
    std::vector<ChatMessage> messages;
    std::vector<std::string> stop_words;
    int32_t max_tokens = -1;        ///< -1 表示不限制
    float temperature = 0.8f;
    float top_p = 0.95f;
    bool stream = true;             ///< 强制流式模式
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

} // namespace workx
