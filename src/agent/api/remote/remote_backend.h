/**
 * @file remote_backend.h
 * @brief 远程后端（OpenAI/Anthropic HTTP/SSE）
 * @details 通过 IProviderAdapter 适配不同 API 协议，使用 HttpClient 发送请求
 * @version 4.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <memory>
#include <atomic>
#include "agent/api/i_backend.h"
#include "agent/api/remote/sse_stream_reader.h"
#include "agent/api/remote/http_client.h"
#include "agent/api/provider/i_provider_adapter.h"

namespace workx {

/// @brief shared_ptr 到 unique_ptr<IStreamReader> 的适配包装
/// @details RemoteBackend 内部用 shared_ptr 管理 SSEStreamReader 生命周期
class SharedPtrWrapper : public IStreamReader {
public:
    explicit SharedPtrWrapper(std::shared_ptr<SSEStreamReader> ptr);
    StreamState next(std::function<bool()> should_stop, StreamChunk& out) override;
    void cancel() override;

private:
    std::shared_ptr<SSEStreamReader> m_ptr;
};

/// @brief 远程后端
/// @details 连接 OpenAI 兼容或 Anthropic API，支持流式推理
class RemoteBackend : public IBackend {
public:
    RemoteBackend() = default;
    ~RemoteBackend() override;

    // IBackend 接口
    std::string name() const override { return "remote"; }
    Result<void, std::string> initialize(const BackendConfig& config) override;
    void shutdown() override;
    bool is_ready() const override { return m_ready.load(); }
    ModelInfo get_model_info() const override;
    Result<std::vector<ModelInfo>, std::string> list_models() override;
    void set_model_name(const std::string& name) override { m_config.model_name = name; }

    // ICompletionProvider 接口
    std::unique_ptr<IStreamReader> submit_completion(const CompletionRequest& request) override;
    void interrupt() override;
    bool is_generating() const override { return m_generating.load(); }

private:
    BackendConfig m_config;
    std::atomic<bool> m_ready{false};
    std::atomic<bool> m_generating{false};

    /// @brief Provider 特定协议适配器
    std::unique_ptr<IProviderAdapter> m_adapter;

    /// @brief HTTP 客户端
    std::unique_ptr<HttpClient> m_http_client;

    // 重试配置
    int m_retry_count = 3;
    int m_retry_delay_ms = 1000;

    // 当前活跃的 reader，用于中断
    std::shared_ptr<SSEStreamReader> m_active_reader;
};

} // namespace workx
