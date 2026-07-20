/**
 * @file openai_adapter.h
 * @brief OpenAI 协议适配器
 * @details 实现 OpenAI chat/completions API 格式
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include "agent/api/provider/i_provider_adapter.h"

namespace agent {

/// @brief OpenAI 协议适配器
/// @details 支持 OpenAI 官方 API 及兼容 API（DeepSeek、Groq、Together 等）
class OpenAIAdapter : public IProviderAdapter {
public:
    ProviderType type() const override { return ProviderType::OpenAI; }

    std::string build_url(const std::string& base_url) const override;

    std::vector<std::pair<std::string, std::string>> build_headers(
        const std::string& api_key) const override;

    std::string build_request_body(const CompletionRequest& request,
                                   const std::string& model_name) const override;

    bool parse_sse_event(const std::string& event_type,
                         const std::string& data,
                         StreamChunk& out) const override;

    /// @brief OpenAI 支持 /v1/models 端点
    ModelEndpointResult get_models_endpoint() const override;
};

} // namespace agent
