/**
 * @file backend_types.h
 * @brief 后端配置与模型信息类型（P1 从 chat_types.h 拆分）
 * @details BackendConfig（后端配置）和 ModelInfo（模型信息）属于"后端管理"领域，
 *          与 ChatMessage/CompletionRequest/StreamChunk 等"聊天消息"领域解耦。
 *          chat_types.h 已 #include 本文件作为向后兼容 shim。
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <cstdint>
#include "agent/model/provider_type.h"

namespace agent {

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
