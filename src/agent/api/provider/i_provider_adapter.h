/**
 * @file i_provider_adapter.h
 * @brief Provider 协议适配器接口
 * @details 封装不同 API 提供商在 URL、认证、请求体、SSE 解析上的差异
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <vector>
#include <utility>
#include "agent/model/provider_type.h"
#include "agent/api/chat_types.h"

namespace workx {

/// @brief Provider 协议适配器接口
/// @details RemoteBackend 通过此接口屏蔽 OpenAI/Anthropic 等 API 差异
class IProviderAdapter {
public:
    virtual ~IProviderAdapter() = default;

    /// @brief 协议类型
    virtual ProviderType type() const = 0;

    /// @brief 构建完整 API URL
    /// @param base_url 基础 URL（不含路径）
    virtual std::string build_url(const std::string& base_url) const = 0;

    /// @brief 构建 HTTP 请求头列表
    /// @param api_key API 密钥
    /// @return header 键值对列表
    virtual std::vector<std::pair<std::string, std::string>> build_headers(const std::string& api_key) const = 0;

    /// @brief 构建请求体 JSON
    /// @param request 推理请求
    /// @param model_name 模型名称
    virtual std::string build_request_body(const CompletionRequest& request,
                                           const std::string& model_name) const = 0;

    /// @brief 解析 SSE 事件
    /// @param event_type SSE event 类型（OpenAI 通常为空）
    /// @param data SSE data 内容（JSON 字符串）
    /// @param out 输出的 StreamChunk
    /// @return true 如果解析出了有效 chunk
    virtual bool parse_sse_event(const std::string& event_type,
                                 const std::string& data,
                                 StreamChunk& out) const = 0;
};

} // namespace workx
