/**
 * @file remote_backend.cpp
 * @brief 远程后端实现
 * @details 使用 HttpClient（Boost.Beast）发送 API 请求，通过 IProviderAdapter 适配不同协议
 * @version 4.0.0
 * @date 2026-07
 */

#include "agent/api/remote/remote_backend.h"
#include "agent/api/provider/openai_adapter.h"
#include "agent/api/provider/anthropic_adapter.h"
#include "core/events/event_bus.h"
#include "agent/message/types.h"

#include <nlohmann/json.hpp>

#include <liblogger/logger.h>

#include <algorithm>
#include <stdexcept>
#include <iostream>

namespace agent {

// ============================================================
// RemoteBackend 实现
// ============================================================

RemoteBackend::~RemoteBackend() {
    shutdown();
}

Result<void, std::string> RemoteBackend::initialize(const BackendConfig& config) {
    if (config.type != BackendConfig::Type::Remote) {
        return Result<void, std::string>::err("RemoteBackend requires Remote config type");
    }
    if (config.base_url.empty()) {
        return Result<void, std::string>::err("base_url is required for RemoteBackend");
    }

    m_config = config;

    // 根据 ProviderType 创建对应的协议适配器
    switch (config.provider) {
        case ProviderType::OpenAI:
            m_adapter = std::make_unique<OpenAIAdapter>();
            break;
        case ProviderType::Anthropic:
            m_adapter = std::make_unique<AnthropicAdapter>();
            break;
        default:
            return Result<void, std::string>::err("Unknown provider type");
    }

#ifdef WORKX_HAS_CURL
    m_http_client = std::make_unique<HttpClient>();
    m_ready.store(true);

    EventBus::instance().publish_async(BackendStatusEvent{
        .status = BackendStatusEvent::Connected,
        .backend_name = name()
    });

    return Result<void, std::string>::ok();
#else
    return Result<void, std::string>::err(
        "RemoteBackend requires CURL. Check CURL installation and reconfigure"
    );
#endif
}

void RemoteBackend::shutdown() {
    if (m_ready.load()) {
        interrupt();
        if (m_http_client) {
            m_http_client->shutdown();
        }
        m_ready.store(false);

        EventBus::instance().publish_async(BackendStatusEvent{
            .status = BackendStatusEvent::Disconnected,
            .backend_name = name()
        });
    }
}

ModelInfo RemoteBackend::get_model_info() const {
    return ModelInfo{
        .name = m_config.model_name,
        .description = "Remote API",
        .context_length = 0
    };
}

std::shared_ptr<IStreamReader> RemoteBackend::submit_completion(const CompletionRequest& request) {
#ifdef WORKX_HAS_CURL
    if (!m_ready.load() || !m_adapter || !m_http_client) {
        return nullptr;
    }

    // 加锁检查在飞请求，避免覆盖旧 reader 导致 HTTP 仍跑但 reader 失联
    std::lock_guard<std::mutex> lock(m_active_mutex);
    if (m_active_reader) {
        // 已有在飞请求，拒绝新请求
        return nullptr;
    }

    m_generating.store(true);

    // 创建 SSE 流读取器，传入 Provider 特定的解析回调
    auto parse_cb = [this](const std::string& event_type,
                           const std::string& data,
                           StreamChunk& out) -> bool {
        return m_adapter->parse_sse_event(event_type, data, out);
    };

    auto reader = std::make_shared<SSEStreamReader>(std::move(parse_cb));
    m_active_reader = reader;

    std::string url = m_adapter->build_url(m_config.base_url);
    std::string body = m_adapter->build_request_body(request, m_config.model_name);
    auto header_pairs = m_adapter->build_headers(m_config.api_key);

    // 通过 HttpClient 发送异步流式 POST
    // on_complete 在 HttpClient poll 线程触发，需加锁与 submit_completion/interrupt 同步
    m_http_client->async_post_stream(
        url, header_pairs, body, reader,
        [this]() {
            std::lock_guard<std::mutex> lock(m_active_mutex);
            m_generating.store(false);
            m_active_reader.reset();
        },
        m_config.timeout_ms);

    // 直接返回 shared_ptr<IStreamReader>，避免 SharedPtrWrapper 适配层
    return reader;

#else
    (void)request;
    return nullptr;
#endif
}

void RemoteBackend::interrupt() {
    std::lock_guard<std::mutex> lock(m_active_mutex);
    if (m_active_reader) {
        m_active_reader->cancel();
        if (m_http_client) {
            m_http_client->cancel_stream(m_active_reader.get());
        }
        m_active_reader.reset();
    }
    m_generating.store(false);
}

// ============================================================
// list_models 实现
// ============================================================

Result<std::vector<ModelInfo>, std::string> RemoteBackend::list_models() {
#ifdef WORKX_HAS_CURL
    if (!m_ready.load() || !m_adapter || !m_http_client) {
        return Result<std::vector<ModelInfo>, std::string>::err("Backend not ready");
    }

    // 检查 provider 是否支持 list_models HTTP 端点
    // Anthropic 无公开 list models 端点，返回内置模型列表
    auto endpoint = m_adapter->get_models_endpoint();
    if (!endpoint.supported) {
        auto builtin = m_adapter->get_builtin_models();
        if (!builtin.empty()) {
            return Result<std::vector<ModelInfo>, std::string>::ok(std::move(builtin));
        }
        return Result<std::vector<ModelInfo>, std::string>::err(
            "This provider does not support list_models endpoint");
    }

    // 构建 URL（用 endpoint.url_suffix，如 "/v1/models"）
    std::string url = m_config.base_url;
    if (url.empty()) {
        return Result<std::vector<ModelInfo>, std::string>::err("base_url is empty");
    }
    while (!url.empty() && url.back() == '/') url.pop_back();
    url += endpoint.url_suffix;

    auto header_pairs = m_adapter->build_headers(m_config.api_key);

    // debug: 记录请求信息
    LOG_INFO("[debug/models] GET {}", url);
    for (const auto& [k, v] : header_pairs) {
        // 安全：对所有可能的认证头脱敏，避免泄露 api key
        std::string k_lower = k;
        std::transform(k_lower.begin(), k_lower.end(), k_lower.begin(), ::tolower);
        std::string val;
        if (k_lower == "authorization" || k_lower == "x-api-key" ||
            k_lower == "x-goog-api-key" || k_lower.find("key") != std::string::npos ||
            k_lower.find("token") != std::string::npos) {
            val = "***";
        } else {
            val = v;
        }
        LOG_INFO("[debug/models]  {}:{} ", k, val);
    }

    // 同步 GET 请求
    auto [status_code, body, error] = m_http_client->get(url, header_pairs, 15000);

    if (!error.empty()) {
        return Result<std::vector<ModelInfo>, std::string>::err(
            std::format("HTTP request failed: {}", error));
    }

    if (status_code >= 400) {
        return Result<std::vector<ModelInfo>, std::string>::err(
            std::format("HTTP error: {} ({})", status_code, body));
    }

    // 解析 JSON 响应
    try {
        auto json = nlohmann::json::parse(body);
        std::vector<ModelInfo> models;

        if (json.contains("data") && json["data"].is_array()) {
            for (const auto& item : json["data"]) {
                ModelInfo info;
                info.name = item.value("id", "unknown");
                info.description = item.value("owned_by", "");
                if (item.contains("context_length") && item["context_length"].is_number()) {
                    info.context_length = item["context_length"].get<int32_t>();
                }
                models.push_back(std::move(info));
            }
        }

        if (models.empty()) {
            return Result<std::vector<ModelInfo>, std::string>::err("No models returned by API");
        }

        return Result<std::vector<ModelInfo>, std::string>::ok(std::move(models));
    } catch (const nlohmann::json::parse_error& e) {
        return Result<std::vector<ModelInfo>, std::string>::err(
            std::format("JSON parse error: {}", e.what()));
    }

#else
    return Result<std::vector<ModelInfo>, std::string>::err(
        "RemoteBackend requires CURL. Check CURL installation and reconfigure");
#endif
}

} // namespace agent
