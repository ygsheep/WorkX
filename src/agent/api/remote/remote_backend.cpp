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
#include "core/config/config_manager.h"
#include "agent/message/types.h"

#ifdef WORKX_HAS_NLOHMANN_JSON
#include <nlohmann/json.hpp>
#endif

#include <liblogger/logger.h>

#include <stdexcept>
#include <iostream>

namespace workx {

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

    // 从 ConfigManager 读取重试配置
    auto& cfg = ConfigManager::instance();
    m_retry_count = cfg.get_or<int>("backend.retry_count", 3);
    m_retry_delay_ms = cfg.get_or<int>("backend.retry_delay_ms", 1000);

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
        "RemoteBackend requires Boost.Beast. Rebuild with -DWORKX_USE_BEAST=ON"
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

std::unique_ptr<IStreamReader> RemoteBackend::submit_completion(const CompletionRequest& request) {
#ifdef WORKX_HAS_CURL
    if (!m_ready.load() || !m_adapter || !m_http_client) {
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
    m_http_client->async_post_stream(
        url, header_pairs, body, reader,
        [this]() {
            m_generating.store(false);
            m_active_reader.reset();
        },
        m_config.timeout_ms);

    return std::unique_ptr<IStreamReader>(new SharedPtrWrapper(reader));

#else
    (void)request;
    return nullptr;
#endif
}

void RemoteBackend::interrupt() {
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

    // 构建 /v1/models URL
    std::string url = m_config.base_url;
    if (url.empty()) {
        return Result<std::vector<ModelInfo>, std::string>::err("base_url is empty");
    }
    while (!url.empty() && url.back() == '/') url.pop_back();
    url += "/v1/models";

    auto header_pairs = m_adapter->build_headers(m_config.api_key);

#ifdef WORKX_HAS_NLOHMANN_JSON
    // debug: 记录请求信息
    LOG_INFO("[debug/models] GET {}", url);
    for (const auto& [k, v] : header_pairs) {
        std::string val = (k == "Authorization") ? "Bearer ***" : v;
        LOG_INFO("[debug/models]  {}:{} ", k, val);
    }
#endif

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
#ifdef WORKX_HAS_NLOHMANN_JSON
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
    return Result<std::vector<ModelInfo>, std::string>::err("JSON library not available");
#endif

#else
    return Result<std::vector<ModelInfo>, std::string>::err(
        "RemoteBackend requires Boost.Beast. Rebuild with -DWORKX_USE_BEAST=ON");
#endif
}

// ============================================================
// SharedPtrWrapper 实现
// ============================================================

SharedPtrWrapper::SharedPtrWrapper(std::shared_ptr<SSEStreamReader> ptr)
    : m_ptr(std::move(ptr)) {}

StreamState SharedPtrWrapper::next(std::function<bool()> should_stop, StreamChunk& out) {
    return m_ptr->next(std::move(should_stop), out);
}

void SharedPtrWrapper::cancel() {
    m_ptr->cancel();
}

} // namespace workx
