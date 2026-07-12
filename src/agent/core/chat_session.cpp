/**
 * @file chat_session.cpp
 * @brief 对话状态机实现
 * @details 编排用户输入、推理、流式事件发布、自动重试、会话持久化
 * @version 3.0.0
 * @date 2026-07
 */

#include "agent/core/chat_session.h"
#include "agent/message/types.h"
#include "agent/api/i_stream_reader.h"
#include "core/task/task_manager.h"
#include "core/config/config_manager.h"

#ifdef WORKX_HAS_NLOHMANN_JSON
#include <nlohmann/json.hpp>
#endif

#include <chrono>
#include <thread>
#include <fstream>

namespace agent {

ChatSession::ChatSession(std::unique_ptr<ICompletionProvider> provider, int retry_delay_ms)
    : m_provider(std::move(provider))
{
    // 从 ConfigManager 读取重试配置，预设值作为 fallback
    auto& cfg = ConfigManager::instance();
    m_max_retries = cfg.get_or<int>("backend.retry_count", 3);
    m_retry_delay_ms = cfg.get_or<int>("backend.retry_delay_ms", retry_delay_ms);

    subscribe_interrupt();
}

ChatSession::~ChatSession() {
    unsubscribe_interrupt();
    if (m_provider) {
        m_provider->interrupt();
    }
}

void ChatSession::set_system_prompt(const std::string& prompt) {
    m_system_prompt = prompt;
}

void ChatSession::clear_history() {
    m_messages.clear();
}

void ChatSession::regenerate() {
    while (!m_messages.empty() &&
           m_messages.back().role == ChatMessage::Role::Assistant) {
        m_messages.pop_back();
    }
    if (!m_messages.empty() &&
        m_messages.back().role == ChatMessage::Role::User) {
        std::string last_user_text = m_messages.back().content;
        m_messages.pop_back();
        run_completion(last_user_text);
    }
}

void ChatSession::send_message(const std::string& text) {
    if (m_generating.load()) {
        EventBus::instance().publish_async(StreamErrorEvent{
            .session_id = "default",
            .message = "Still generating, please wait or press Ctrl+C to interrupt",
            .retryable = true
        });
        return;
    }

    run_completion(text);
}

void ChatSession::run_completion(const std::string& user_text, int retry_attempt) {
    // 仅首次请求时添加用户消息（重试时不重复添加）
    if (retry_attempt == 0) {
        m_messages.push_back(ChatMessage::user(user_text));
    }

    CompletionRequest request;
    request.stream = true;

    if (!m_system_prompt.empty()) {
        request.messages.push_back(ChatMessage::system(m_system_prompt));
    }

    for (const auto& msg : m_messages) {
        request.messages.push_back(msg);
    }

    EventBus::instance().publish_async(BackendStatusEvent{
        .status = BackendStatusEvent::Connecting,
        .backend_name = "session"
    });

    m_generating.store(true);

    int max_retries = m_max_retries;
    int retry_delay_ms = m_retry_delay_ms;

    auto task = TaskManager::instance().launch("completion",
        [this, request = std::move(request), retry_attempt, max_retries, retry_delay_ms, user_text]
        (const std::atomic<bool>& should_cancel) {
            auto reader = m_provider->submit_completion(request);
            if (!reader) {
                // 提交失败，尝试重试
                if (retry_attempt < max_retries) {
                    int delay = retry_delay_ms * (1 << retry_attempt);
                    EventBus::instance().publish_async(StreamErrorEvent{
                        .session_id = "default",
                        .message = std::format("Submit failed, retrying in {}ms... ({}/{})",
                                               delay, retry_attempt + 1, max_retries),
                        .retryable = true
                    });

                    // 可中断的等待
                    auto wait_until = std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds(delay);
                    while (std::chrono::steady_clock::now() < wait_until) {
                        if (should_cancel) {
                            m_generating.store(false);
                            return;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }

                    if (!should_cancel) {
                        run_completion(user_text, retry_attempt + 1);
                    }
                } else {
                    EventBus::instance().publish_async(StreamErrorEvent{
                        .session_id = "default",
                        .message = "Failed to submit completion request after retries",
                        .retryable = false
                    });
                    m_generating.store(false);
                }
                return;
            }

            StreamChunk chunk;
            std::string full_content;
            std::string full_reasoning;
            auto start_time = std::chrono::steady_clock::now();

            while (true) {
                if (should_cancel) {
                    reader->cancel();
                    break;
                }

                auto state = reader->next(
                    [&should_cancel]() { return should_cancel.load(); },
                    chunk
                );

                if (state == StreamState::HasData) {
                    if (!chunk.content_delta.empty() || !chunk.reasoning_delta.empty()) {
                        full_content += chunk.content_delta;
                        full_reasoning += chunk.reasoning_delta;

                        EventBus::instance().publish_async(StreamTokenEvent{
                            .session_id = "default",
                            .content_delta = chunk.content_delta,
                            .reasoning_delta = chunk.reasoning_delta,
                            .is_thinking = !chunk.reasoning_delta.empty(),
                            .token_count = chunk.token_count
                        });
                    }
                } else if (state == StreamState::Complete) {
                    if (!chunk.content_delta.empty() || !chunk.reasoning_delta.empty()) {
                        full_content += chunk.content_delta;
                        full_reasoning += chunk.reasoning_delta;
                    }

                    auto end_time = std::chrono::steady_clock::now();
                    double elapsed_ms = std::chrono::duration<double, std::milli>(
                        end_time - start_time).count();

                    m_messages.push_back(ChatMessage::assistant(full_content));
                    if (!full_reasoning.empty()) {
                        m_messages.back().reasoning_content = full_reasoning;
                    }

                    EventBus::instance().publish_async(StreamDoneEvent{
                        .session_id = "default",
                        .full_content = full_content,
                        .full_reasoning = full_reasoning,
                        .was_interrupted = false,
                        .prompt_tokens = chunk.prompt_tokens,
                        .generated_tokens = chunk.generated_tokens,
                        .prompt_ms = chunk.prompt_ms,
                        .generation_ms = elapsed_ms
                    });
                    break;
                } else if (state == StreamState::Error) {
                    // 流式错误，尝试重试
                    if (retry_attempt < max_retries) {
                        int delay = retry_delay_ms * (1 << retry_attempt);
                        EventBus::instance().publish_async(StreamErrorEvent{
                            .session_id = "default",
                            .message = std::format("Stream error, retrying in {}ms... ({}/{})",
                                                   delay, retry_attempt + 1, max_retries),
                            .retryable = true
                        });

                        auto wait_until = std::chrono::steady_clock::now() +
                                          std::chrono::milliseconds(delay);
                        while (std::chrono::steady_clock::now() < wait_until) {
                            if (should_cancel) {
                                m_generating.store(false);
                                return;
                            }
                            std::this_thread::sleep_for(std::chrono::milliseconds(100));
                        }

                        if (!should_cancel) {
                            // 移除可能部分添加的 assistant 消息
                            while (!m_messages.empty() &&
                                   m_messages.back().role == ChatMessage::Role::Assistant) {
                                m_messages.pop_back();
                            }
                            run_completion(user_text, retry_attempt + 1);
                        }
                    } else {
                        EventBus::instance().publish_async(StreamErrorEvent{
                            .session_id = "default",
                            .message = "Stream error occurred after retries",
                            .retryable = false
                        });
                    }
                    break;
                } else if (state == StreamState::Cancelled) {
                    if (!full_content.empty()) {
                        m_messages.push_back(ChatMessage::assistant(full_content));
                    }

                    EventBus::instance().publish_async(StreamDoneEvent{
                        .session_id = "default",
                        .full_content = full_content,
                        .full_reasoning = full_reasoning,
                        .was_interrupted = true,
                        .prompt_tokens = 0,
                        .generated_tokens = 0,
                        .prompt_ms = 0.0,
                        .generation_ms = 0.0
                    });
                    break;
                }
            }

            m_generating.store(false);
        },
        TaskType::Normal
    );
}

// ============================================================
// 会话持久化
// ============================================================

Result<void, std::string> ChatSession::save_session(const std::string& path) const {
#ifdef WORKX_HAS_NLOHMANN_JSON
    nlohmann::json j;

    if (!m_system_prompt.empty()) {
        j["system_prompt"] = m_system_prompt;
    }

    auto& messages = j["messages"];
    for (const auto& msg : m_messages) {
        nlohmann::json m;
        switch (msg.role) {
            case ChatMessage::Role::System:    m["role"] = "system"; break;
            case ChatMessage::Role::User:      m["role"] = "user"; break;
            case ChatMessage::Role::Assistant: m["role"] = "assistant"; break;
            case ChatMessage::Role::Tool:      m["role"] = "tool"; break;
        }
        m["content"] = msg.content;
        if (!msg.reasoning_content.empty()) {
            m["reasoning_content"] = msg.reasoning_content;
        }
        if (msg.role == ChatMessage::Role::Tool) {
            m["tool_call_id"] = msg.tool_call_id;
            m["tool_name"] = msg.tool_name;
        }
        messages.push_back(m);
    }

    try {
        std::filesystem::path p(path);
        if (p.has_parent_path()) {
            std::filesystem::create_directories(p.parent_path());
        }

        std::ofstream file(path);
        if (!file.is_open()) {
            return Result<void, std::string>::err(
                std::format("Failed to create file: {}", path)
            );
        }
        file << j.dump(2);
        file.close();
        return Result<void, std::string>::ok();

    } catch (const std::exception& e) {
        return Result<void, std::string>::err(
            std::format("Error saving session: {}", e.what())
        );
    }
#else
    (void)path;
    return Result<void, std::string>::err("Session persistence requires nlohmann/json");
#endif
}

Result<void, std::string> ChatSession::load_session(const std::string& path) {
#ifdef WORKX_HAS_NLOHMANN_JSON
    if (!std::filesystem::exists(path)) {
        return Result<void, std::string>::err(
            std::format("File not found: {}", path)
        );
    }

    try {
        std::ifstream file(path);
        if (!file.is_open()) {
            return Result<void, std::string>::err(
                std::format("Failed to open file: {}", path)
            );
        }

        nlohmann::json j;
        file >> j;
        file.close();

        m_messages.clear();

        if (j.contains("system_prompt")) {
            m_system_prompt = j["system_prompt"].get<std::string>();
        }

        if (j.contains("messages")) {
            for (const auto& m : j["messages"]) {
                ChatMessage msg;
                std::string role_str = m["role"].get<std::string>();
                if (role_str == "system")       msg.role = ChatMessage::Role::System;
                else if (role_str == "user")    msg.role = ChatMessage::Role::User;
                else if (role_str == "assistant") msg.role = ChatMessage::Role::Assistant;
                else if (role_str == "tool")    msg.role = ChatMessage::Role::Tool;

                msg.content = m["content"].get<std::string>();
                if (m.contains("reasoning_content")) {
                    msg.reasoning_content = m["reasoning_content"].get<std::string>();
                }
                if (msg.role == ChatMessage::Role::Tool) {
                    if (m.contains("tool_call_id")) {
                        msg.tool_call_id = m["tool_call_id"].get<std::string>();
                    }
                    if (m.contains("tool_name")) {
                        msg.tool_name = m["tool_name"].get<std::string>();
                    }
                }
                m_messages.push_back(std::move(msg));
            }
        }

        return Result<void, std::string>::ok();

    } catch (const nlohmann::json::parse_error& e) {
        return Result<void, std::string>::err(
            std::format("JSON parse error: {}", e.what())
        );
    } catch (const std::exception& e) {
        return Result<void, std::string>::err(
            std::format("Error loading session: {}", e.what())
        );
    }
#else
    (void)path;
    return Result<void, std::string>::err("Session persistence requires nlohmann/json");
#endif
}

// ============================================================
// 事件订阅
// ============================================================

void ChatSession::subscribe_interrupt() {
    m_interrupt_token = EventBus::instance().subscribe<InterruptEvent>(
        [this](const InterruptEvent& /*e*/) {
            if (m_provider) {
                m_provider->interrupt();
            }
        }
    );
}

void ChatSession::unsubscribe_interrupt() {
    EventBus::instance().unsubscribe<InterruptEvent>(m_interrupt_token);
}

} // namespace agent
