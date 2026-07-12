/**
 * @file client.h
 * @brief Client — agent/api 顶层封装入口
 * @details 收拢 BackendFactory + RemoteBackend + 会话管理为一站式 API。
 *          支持 ClientConfig 指定初始化器构造、阻塞/异步两套聊天 API、
 *          可选 EventBus 集成（兼容现有 TUI ChatRenderer）。
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/utils/result.h"
#include "core/events/event_bus.h"
#include "agent/api/chat_types.h"
#include "agent/model/provider_type.h"

namespace agent {

class IBackend;

// ============================================================
// ChatCallbacks
// ============================================================

/// @brief 流式回调集合
struct ChatCallbacks {
    /// @brief 收到增量 token（回复或思考）
    /// @param content_delta 正文增量
    /// @param reasoning_delta 推理/思考增量
    std::function<void(const std::string& content_delta,
                       const std::string& reasoning_delta)> on_token;

    /// @brief 流式完成（正常结束），final_chunk.is_final == true
    std::function<void(const StreamChunk& final_chunk)> on_done;

    /// @brief 流途中错误（提交成功后的流错误）
    /// @param msg 错误信息
    /// @param retryable 是否可重试
    std::function<void(const std::string& msg, bool retryable)> on_error;
};

// ============================================================
// ClientConfig
// ============================================================

/// @brief Client 配置（聚合体，支持指定初始化器）
/// @details 用法：Client::create({.provider = "lm-studio", .model = "..."})
struct ClientConfig {
    /// Provider 预设名（如 "lm-studio"/"deepseek"/"openai"）。
    /// 非空时自动查 ProviderPreset 填充 backend 的 base_url/provider/默认 model。
    /// 留空时所有 backend 字段需手动设置。
    std::string provider;

    /// 模型名（显式设置优先于 preset 默认值，覆盖 backend.model_name）
    std::string model;

    /// 内层后端配置（base_url / api_key / timeout_ms / provider 等）
    BackendConfig backend;

    /// 系统提示词（可选）
    std::string system_prompt;

    /// 重试次数与初始退避延迟（毫秒，指数退避）
    int retry_count = 3;
    int retry_delay_ms = 1000;

    /// 开启后：订阅 InterruptEvent 自动中断 + 发布 StreamToken/Done/Error
    bool enable_event_bus = false;
};

// ============================================================
// Client
// ============================================================

/// @brief 顶层聊天客户端 — 封装 BackendFactory + 会话管理
/// @details 调用方不直接接触 IBackend / ChatSession / EventBus。
class Client {
public:
    /// @brief 从配置创建 Client
    /// @details 内部完成：查 preset（若 provider 非空）→ 拼 BackendConfig
    ///          → BackendFactory::create → backend->initialize
    /// @return 失败返回错误信息（不抛异常）
    /// @code
    /// auto r = Client::create({.provider = "lm-studio",
    ///                          .model = "google/gemma-4-e4b"});
    /// if (r.isErr()) { std::cerr << r.error(); return; }
    /// Client client = std::move(r.unwrap());
    /// @endcode
    static Result<Client, std::string> create(ClientConfig config);

    // ---- 会话管理 ----
    void set_system_prompt(const std::string& prompt);
    void clear_history();
    void regenerate();
    [[nodiscard]] const std::vector<ChatMessage>& history() const;

    // ---- 阻塞 API（脚本/CLI 场景）----
    // 在调用线程同步执行，回调在该线程触发。调用期间线程被阻塞。
    // 适合一次性脚本；TUI 主线程勿用（会冻结界面）。
    Result<std::string, std::string> chat(const std::string& user_text);
    Result<std::string, std::string> chat(const std::vector<ChatMessage>& messages);
    Result<void, std::string> stream_chat(const std::string& user_text,
                                          const ChatCallbacks& cbs);
    Result<void, std::string> stream_chat(const std::vector<ChatMessage>& messages,
                                          const ChatCallbacks& cbs);

    // ---- 异步 API（TUI/交互场景）----
    // 立即返回，推理在 TaskManager 后台任务中执行。
    // 回调在后台线程触发 → 调用方需处理线程安全。
    // TUI 场景建议开启 enable_event_bus，用 EventBus 回主线程渲染。
    // 返回 Ok 表示已提交；提交级失败返回 Err。
    Result<void, std::string> chat_async(const std::string& user_text,
                                         const ChatCallbacks& cbs);
    Result<void, std::string> stream_chat_async(const std::string& user_text,
                                                const ChatCallbacks& cbs);

    // ---- 控制 ----
    void interrupt();
    [[nodiscard]] bool is_generating() const;

    // ---- 后端能力透传 ----
    Result<std::vector<ModelInfo>, std::string> list_models();
    void set_model(const std::string& name);
    [[nodiscard]] std::string model_name() const;

    // 仅可移动（手动实现，因 std::atomic 不可移动）
    Client(Client&& other) noexcept;
    Client& operator=(Client&& other) noexcept;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    ~Client();

private:
    Client(std::unique_ptr<IBackend> backend,
           std::string system_prompt,
           int retry_count,
           int retry_delay_ms,
           bool publish_events);

    /// 构造 CompletionRequest（含 system_prompt + history + 新消息）
    CompletionRequest build_request(const std::string& user_text);
    CompletionRequest build_request(const std::vector<ChatMessage>& messages);

    /// 流式核心实现（阻塞版共享）
    /// @param should_stop 外部取消检查
    Result<void, std::string> run_stream(const CompletionRequest& request,
                                         const ChatCallbacks& cbs,
                                         const std::function<bool()>& should_stop,
                                         std::string& content_out,
                                         std::string& reasoning_out);

    std::unique_ptr<IBackend> m_backend;
    std::vector<ChatMessage> m_messages;
    std::string m_system_prompt;
    std::atomic<bool> m_generating{false};
    int m_max_retries = 3;
    int m_retry_delay_ms = 1000;
    bool m_publish_events = false;

    /// EventBus 订阅 token（仅 m_publish_events=true 时有效）
    EventToken m_interrupt_token;
    bool m_subscribed = false;
};

} // namespace agent
