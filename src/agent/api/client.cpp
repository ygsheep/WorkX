/**
 * @file client.cpp
 * @brief Client 实现 — agent/api 顶层封装
 * @version 1.0.0
 * @date 2026-07
 */

#include "agent/api/client.h"

#include <chrono>
#include <thread>

#include "agent/api/i_backend.h"
#include "agent/api/backend_factory.h"
#include "agent/model/provider_preset.h"
#include "core/task/task_manager.h"
#include "core/events/event_bus.h"
#include "agent/message/types.h"

namespace agent {

// ============================================================
// create() — 静态工厂
// ============================================================

Result<Client, std::string> Client::create(ClientConfig cfg) {
    // 1. 若指定了 provider preset，查表填充 backend 默认值
    if (!cfg.provider.empty()) {
        const auto* preset = find_preset(cfg.provider);
        if (!preset) {
            return Result<Client, std::string>::err(
                "Unknown provider preset: " + cfg.provider);
        }
        if (cfg.backend.base_url.empty()) {
            cfg.backend.base_url = std::string(preset->default_url);
        }
        cfg.backend.provider = preset->type;
        if (cfg.model.empty()) {
            cfg.model = std::string(preset->default_model);
        }
    }

    // 2. 模型名覆盖
    if (!cfg.model.empty()) {
        cfg.backend.model_name = cfg.model;
    }

    // 3. 校验必要字段
    if (cfg.backend.base_url.empty()) {
        return Result<Client, std::string>::err("base_url is required");
    }
    cfg.backend.type = BackendConfig::Type::Remote;

    // 4. 创建后端
    auto backend = BackendFactory::create(cfg.backend);
    if (!backend) {
        return Result<Client, std::string>::err("Failed to create backend");
    }

    // 5. 初始化后端
    auto init_result = backend->initialize(cfg.backend);
    if (init_result.isErr()) {
        return Result<Client, std::string>::err(init_result.error());
    }

    // 6. 构造 Client
    Client client(std::move(backend), cfg.system_prompt,
                  cfg.retry_count, cfg.retry_delay_ms, cfg.enable_event_bus);
    return Result<Client, std::string>::ok(std::move(client));
}

// ============================================================
// 构造 / 析构 / 移动
// ============================================================

Client::Client(std::unique_ptr<IBackend> backend,
               std::string system_prompt,
               int retry_count,
               int retry_delay_ms,
               bool publish_events)
    : m_backend(std::move(backend))
    , m_system_prompt(std::move(system_prompt))
    , m_max_retries(retry_count)
    , m_retry_delay_ms(retry_delay_ms)
    , m_publish_events(publish_events)
{
    if (m_publish_events) {
        // 订阅 InterruptEvent → 自动中断
        m_interrupt_token = EventBus::instance().subscribe<InterruptEvent>(
            [this](const InterruptEvent&) {
                if (m_backend) {
                    m_backend->interrupt();
                }
            });
        m_subscribed = true;
    }
}

Client::~Client() {
    if (m_subscribed && m_interrupt_token.is_valid()) {
        EventBus::instance().unsubscribe<InterruptEvent>(m_interrupt_token);
    }
    if (m_backend) {
        m_backend->interrupt();
        m_backend->shutdown();
    }
}

Client::Client(Client&& other) noexcept
    : m_backend(std::move(other.m_backend))
    , m_messages(std::move(other.m_messages))
    , m_system_prompt(std::move(other.m_system_prompt))
    , m_generating(other.m_generating.load())
    , m_max_retries(other.m_max_retries)
    , m_retry_delay_ms(other.m_retry_delay_ms)
    , m_publish_events(other.m_publish_events)
    , m_interrupt_token(std::move(other.m_interrupt_token))
    , m_subscribed(other.m_subscribed)
{
    other.m_subscribed = false;
    other.m_generating.store(false);
}

Client& Client::operator=(Client&& other) noexcept {
    if (this != &other) {
        // 清理当前订阅
        if (m_subscribed && m_interrupt_token.is_valid()) {
            EventBus::instance().unsubscribe<InterruptEvent>(m_interrupt_token);
        }
        if (m_backend) {
            m_backend->interrupt();
            m_backend->shutdown();
        }

        m_backend = std::move(other.m_backend);
        m_messages = std::move(other.m_messages);
        m_system_prompt = std::move(other.m_system_prompt);
        m_generating.store(other.m_generating.load());
        m_max_retries = other.m_max_retries;
        m_retry_delay_ms = other.m_retry_delay_ms;
        m_publish_events = other.m_publish_events;
        m_interrupt_token = std::move(other.m_interrupt_token);
        m_subscribed = other.m_subscribed;

        other.m_subscribed = false;
        other.m_generating.store(false);
    }
    return *this;
}

// ============================================================
// build_request — 构造 CompletionRequest
// ============================================================

CompletionRequest Client::build_request(const std::string& user_text) {
    CompletionRequest req;
    if (!m_system_prompt.empty()) {
        req.messages.push_back(ChatMessage::system(m_system_prompt));
    }
    req.messages.insert(req.messages.end(), m_messages.begin(), m_messages.end());
    req.messages.push_back(ChatMessage::user(user_text));
    return req;
}

CompletionRequest Client::build_request(const std::vector<ChatMessage>& messages) {
    CompletionRequest req;
    if (!m_system_prompt.empty()) {
        req.messages.push_back(ChatMessage::system(m_system_prompt));
    }
    req.messages.insert(req.messages.end(), messages.begin(), messages.end());
    return req;
}

// ============================================================
// run_stream — 核心流式逻辑（阻塞版共享）
// ============================================================

Result<void, std::string> Client::run_stream(
    const CompletionRequest& request,
    const ChatCallbacks& cbs,
    const std::function<bool()>& should_stop,
    std::string& content_out,
    std::string& reasoning_out)
{
    content_out.clear();
    reasoning_out.clear();

    for (int attempt = 0; attempt <= m_max_retries; ++attempt) {
        if (should_stop()) {
            return Result<void, std::string>::ok();
        }

        auto reader = m_backend->submit_completion(request);
        if (!reader) {
            // 提交失败，尝试重试
            if (attempt < m_max_retries) {
                int delay = m_retry_delay_ms * (1 << attempt);
                if (cbs.on_error) {
                    cbs.on_error(
                        "Submit failed, retrying in " + std::to_string(delay) + "ms... ("
                        + std::to_string(attempt + 1) + "/" + std::to_string(m_max_retries) + ")",
                        true);
                }
                if (m_publish_events) {
                    EventBus::instance().publish_async(StreamErrorEvent{
                        .session_id = "client",
                        .message = "Submit failed, retrying...",
                        .retryable = true
                    });
                }
                // 可中断的退避等待
                auto wait_until = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(delay);
                while (std::chrono::steady_clock::now() < wait_until) {
                    if (should_stop()) {
                        return Result<void, std::string>::ok();
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                continue;
            }
            // 重试耗尽
            if (cbs.on_error) {
                cbs.on_error("Failed to submit completion request after retries", false);
            }
            if (m_publish_events) {
                EventBus::instance().publish_async(StreamErrorEvent{
                    .session_id = "client",
                    .message = "Failed to submit completion request after retries",
                    .retryable = false
                });
            }
            return Result<void, std::string>::err(
                "Failed to submit completion request after retries");
        }

        // 拉取流式响应
        StreamChunk chunk;
        while (true) {
            if (should_stop()) {
                reader->cancel();
                break;
            }

            auto state = reader->next(should_stop, chunk);

            if (state == StreamState::HasData) {
                if (!chunk.content_delta.empty() || !chunk.reasoning_delta.empty()) {
                    content_out += chunk.content_delta;
                    reasoning_out += chunk.reasoning_delta;

                    if (cbs.on_token) {
                        cbs.on_token(chunk.content_delta, chunk.reasoning_delta);
                    }
                    if (m_publish_events) {
                        EventBus::instance().publish_async(StreamTokenEvent{
                            .session_id = "client",
                            .content_delta = chunk.content_delta,
                            .reasoning_delta = chunk.reasoning_delta,
                            .is_thinking = !chunk.reasoning_delta.empty(),
                            .token_count = chunk.token_count
                        });
                    }
                }
            } else if (state == StreamState::Complete) {
                // 处理可能的最后一块数据
                if (!chunk.content_delta.empty() || !chunk.reasoning_delta.empty()) {
                    content_out += chunk.content_delta;
                    reasoning_out += chunk.reasoning_delta;
                    if (cbs.on_token) {
                        cbs.on_token(chunk.content_delta, chunk.reasoning_delta);
                    }
                }
                if (cbs.on_done) {
                    cbs.on_done(chunk);
                }
                if (m_publish_events) {
                    EventBus::instance().publish_async(StreamDoneEvent{
                        .session_id = "client",
                        .full_content = content_out,
                        .full_reasoning = reasoning_out,
                        .was_interrupted = false,
                        .prompt_tokens = chunk.prompt_tokens,
                        .generated_tokens = chunk.generated_tokens,
                        .prompt_ms = chunk.prompt_ms,
                        .generation_ms = chunk.generation_ms
                    });
                }
                return Result<void, std::string>::ok();
            } else if (state == StreamState::Cancelled) {
                if (cbs.on_done) {
                    StreamChunk final_chunk = chunk;
                    final_chunk.is_final = true;
                    cbs.on_done(final_chunk);
                }
                if (m_publish_events) {
                    EventBus::instance().publish_async(StreamDoneEvent{
                        .session_id = "client",
                        .full_content = content_out,
                        .full_reasoning = reasoning_out,
                        .was_interrupted = true
                    });
                }
                return Result<void, std::string>::ok();
            } else if (state == StreamState::Error) {
                // 流错误，尝试重试
                if (attempt < m_max_retries) {
                    int delay = m_retry_delay_ms * (1 << attempt);
                    if (cbs.on_error) {
                        cbs.on_error("Stream error, retrying...", true);
                    }
                    auto wait_until = std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds(delay);
                    while (std::chrono::steady_clock::now() < wait_until) {
                        if (should_stop()) {
                            return Result<void, std::string>::ok();
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                    break;  // break inner while, continue retry loop
                }
                if (cbs.on_error) {
                    cbs.on_error("Stream error after retries", false);
                }
                if (m_publish_events) {
                    EventBus::instance().publish_async(StreamErrorEvent{
                        .session_id = "client",
                        .message = "Stream error after retries",
                        .retryable = false
                    });
                }
                return Result<void, std::string>::err("Stream error after retries");
            }
        }
    }

    return Result<void, std::string>::ok();
}

// ============================================================
// 阻塞 API
// ============================================================

Result<std::string, std::string> Client::chat(const std::string& user_text) {
    auto request = build_request(user_text);
    m_messages.push_back(ChatMessage::user(user_text));

    m_generating.store(true);
    std::string content, reasoning;
    auto result = run_stream(request, {}, []() { return false; }, content, reasoning);
    m_generating.store(false);

    if (result.isOk()) {
        m_messages.push_back(ChatMessage::assistant(content));
        return Result<std::string, std::string>::ok(std::move(content));
    }
    return Result<std::string, std::string>::err(result.error());
}

Result<std::string, std::string> Client::chat(const std::vector<ChatMessage>& messages) {
    auto request = build_request(messages);

    m_generating.store(true);
    std::string content, reasoning;
    auto result = run_stream(request, {}, []() { return false; }, content, reasoning);
    m_generating.store(false);

    if (result.isOk()) {
        return Result<std::string, std::string>::ok(std::move(content));
    }
    return Result<std::string, std::string>::err(result.error());
}

Result<void, std::string> Client::stream_chat(const std::string& user_text,
                                               const ChatCallbacks& cbs) {
    auto request = build_request(user_text);
    m_messages.push_back(ChatMessage::user(user_text));

    m_generating.store(true);
    std::string content, reasoning;
    auto result = run_stream(request, cbs, []() { return false; }, content, reasoning);
    m_generating.store(false);

    if (result.isOk()) {
        m_messages.push_back(ChatMessage::assistant(content));
    }
    return result;
}

Result<void, std::string> Client::stream_chat(const std::vector<ChatMessage>& messages,
                                               const ChatCallbacks& cbs) {
    auto request = build_request(messages);

    m_generating.store(true);
    std::string content, reasoning;
    auto result = run_stream(request, cbs, []() { return false; }, content, reasoning);
    m_generating.store(false);

    return result;
}

// ============================================================
// 异步 API
// ============================================================

Result<void, std::string> Client::chat_async(const std::string& user_text,
                                              const ChatCallbacks& cbs) {
    // 聚合 content 的回调
    auto aggregated_cbs = cbs;
    auto content_ptr = std::make_shared<std::string>();
    auto reasoning_ptr = std::make_shared<std::string>();

    auto original_on_done = cbs.on_done;
    auto content_ptr_copy = content_ptr;
    auto reasoning_ptr_copy = reasoning_ptr;

    aggregated_cbs.on_token = [cbs, content_ptr, reasoning_ptr](
        const std::string& content_delta, const std::string& reasoning_delta) {
        *content_ptr += content_delta;
        *reasoning_ptr += reasoning_delta;
        if (cbs.on_token) {
            cbs.on_token(content_delta, reasoning_delta);
        }
    };

    aggregated_cbs.on_done = [original_on_done, content_ptr_copy, reasoning_ptr_copy, this](
        const StreamChunk& final_chunk) {
        // 记录 assistant 消息到 history
        m_messages.push_back(ChatMessage::assistant(*content_ptr_copy));
        if (original_on_done) {
            original_on_done(final_chunk);
        }
    };

    return stream_chat_async(user_text, aggregated_cbs);
}

Result<void, std::string> Client::stream_chat_async(const std::string& user_text,
                                                     const ChatCallbacks& cbs) {
    if (m_generating.load()) {
        return Result<void, std::string>::err("Already generating");
    }

    auto request = build_request(user_text);
    m_messages.push_back(ChatMessage::user(user_text));

    m_generating.store(true);

    // 捕获 content 用于记录 assistant 消息
    auto content_ptr = std::make_shared<std::string>();
    auto reasoning_ptr = std::make_shared<std::string>();

    ChatCallbacks wrapped_cbs;
    wrapped_cbs.on_token = [cbs, content_ptr, reasoning_ptr](
        const std::string& content_delta, const std::string& reasoning_delta) {
        *content_ptr += content_delta;
        *reasoning_ptr += reasoning_delta;
        if (cbs.on_token) {
            cbs.on_token(content_delta, reasoning_delta);
        }
    };
    auto original_on_done = cbs.on_done;
    wrapped_cbs.on_done = [original_on_done, content_ptr, this](
        const StreamChunk& final_chunk) {
        m_messages.push_back(ChatMessage::assistant(*content_ptr));
        m_generating.store(false);
        if (original_on_done) {
            original_on_done(final_chunk);
        }
    };
    auto original_on_error = cbs.on_error;
    wrapped_cbs.on_error = [original_on_error, this](
        const std::string& msg, bool retryable) {
        m_generating.store(false);
        if (original_on_error) {
            original_on_error(msg, retryable);
        }
    };

    // 启动后台任务
    TaskManager::instance().launch("client_completion",
        [this, request = std::move(request), cbs = std::move(wrapped_cbs),
         content_ptr, reasoning_ptr]
        (const std::atomic<bool>& should_cancel) {
            std::string content, reasoning;
            run_stream(request, cbs,
                       [&should_cancel]() { return should_cancel.load(); },
                       content, reasoning);
            // 确保 generating 被重置（on_done/on_error 可能未触发）
            m_generating.store(false);
        });

    return Result<void, std::string>::ok();
}

// ============================================================
// 会话管理
// ============================================================

void Client::set_system_prompt(const std::string& prompt) {
    m_system_prompt = prompt;
}

void Client::clear_history() {
    m_messages.clear();
}

void Client::regenerate() {
    // 移除最后一条 assistant 消息
    if (!m_messages.empty() && m_messages.back().role == ChatMessage::Role::Assistant) {
        m_messages.pop_back();
    }
}

const std::vector<ChatMessage>& Client::history() const {
    return m_messages;
}

// ============================================================
// 控制
// ============================================================

void Client::interrupt() {
    if (m_backend) {
        m_backend->interrupt();
    }
}

bool Client::is_generating() const {
    return m_generating.load();
}

// ============================================================
// 后端能力透传
// ============================================================

Result<std::vector<ModelInfo>, std::string> Client::list_models() {
    return m_backend->list_models();
}

void Client::set_model(const std::string& name) {
    m_backend->set_model_name(name);
}

std::string Client::model_name() const {
    return m_backend->get_model_info().name;
}

} // namespace agent
