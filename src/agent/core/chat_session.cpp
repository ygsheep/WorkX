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

void ChatSession::set_tool_registry(std::shared_ptr<tool::ToolRegistry> registry) {
    m_tool_registry = std::move(registry);
    if (m_tool_registry) {
        m_tool_executor = std::make_unique<tool::ToolExecutor>(m_tool_registry);
    } else {
        m_tool_executor.reset();
    }
}

CompletionRequest ChatSession::build_request() const {
    CompletionRequest request;
    request.stream = true;

    if (!m_system_prompt.empty()) {
        request.messages.push_back(ChatMessage::system(m_system_prompt));
    }

    for (const auto& msg : m_messages) {
        request.messages.push_back(msg);
    }

    // 注入工具 schema（启用 function calling）
    if (m_tool_registry && m_tool_registry->size() > 0) {
        request.tools = m_tool_registry->get_all_schemas();
    }

    return request;
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

    EventBus::instance().publish_async(BackendStatusEvent{
        .status = BackendStatusEvent::Connecting,
        .backend_name = "session"
    });

    m_generating.store(true);

    int max_retries = m_max_retries;
    int retry_delay_ms = m_retry_delay_ms;

    auto task = TaskManager::instance().launch("completion",
        [this, retry_attempt, max_retries, retry_delay_ms, user_text]
        (const std::atomic<bool>& should_cancel) {
            // ---- agent 循环（LLM → tool_use → execute → tool_result → LLM）----
            const int max_iterations = 25;
            int iteration = 0;

            while (iteration < max_iterations) {
                ++iteration;

                // 每次 iteration 重建 request（messages 可能因 tool_result 增长）
                CompletionRequest request = build_request();

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
                    }
                    m_generating.store(false);
                    return;
                }

                // ---- 流式读取（本轮 LLM 响应）----
                struct PendingToolUse {
                    std::string id;
                    std::string name;
                    std::string input_json;
                };
                std::vector<PendingToolUse> pending_tools;
                bool stream_completed = false;
                bool stream_error = false;
                bool stream_cancelled = false;

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
                        // 文本/推理增量
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

                        // tool_use content_block 开始
                        if (chunk.is_tool_use_start) {
                            pending_tools.push_back({
                                chunk.tool_use_id,
                                chunk.tool_name,
                                ""
                            });
                        }

                        // tool_use input JSON 增量（流式拼接）
                        if (chunk.is_tool_use_delta && !pending_tools.empty()) {
                            pending_tools.back().input_json += chunk.tool_input_delta;
                        }
                    } else if (state == StreamState::Complete) {
                        if (!chunk.content_delta.empty() || !chunk.reasoning_delta.empty()) {
                            full_content += chunk.content_delta;
                            full_reasoning += chunk.reasoning_delta;
                        }
                        stream_completed = true;
                        break;
                    } else if (state == StreamState::Error) {
                        stream_error = true;
                        break;
                    } else if (state == StreamState::Cancelled) {
                        stream_cancelled = true;
                        break;
                    }
                }

                // ---- 处理取消 ----
                if (stream_cancelled) {
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
                    m_generating.store(false);
                    return;
                }

                // ---- 处理流式错误（重试整个 agent 循环）----
                if (stream_error) {
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
                    m_generating.store(false);
                    return;
                }

                // ---- 流式完成 ----
                auto end_time = std::chrono::steady_clock::now();
                double elapsed_ms = std::chrono::duration<double, std::milli>(
                    end_time - start_time).count();

                // 没有 tool_use：正常完成，发布 done 事件并退出 agent 循环
                if (pending_tools.empty()) {
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
                    m_generating.store(false);
                    return;
                }

                // ---- 有 tool_use：执行工具并继续 agent 循环 ----
                // 1. 构建 assistant 消息（包含 text + tool_uses）
                ChatMessage assistant_msg = ChatMessage::assistant(full_content);
                if (!full_reasoning.empty()) {
                    assistant_msg.reasoning_content = full_reasoning;
                }
                for (const auto& ptu : pending_tools) {
                    ToolUse tu;
                    tu.id = ptu.id;
                    tu.name = ptu.name;
                    try {
                        tu.input = ptu.input_json.empty()
                            ? nlohmann::json::object()
                            : nlohmann::json::parse(ptu.input_json);
                    } catch (const nlohmann::json::parse_error&) {
                        tu.input = nlohmann::json::object();
                    }
                    assistant_msg.tool_uses.push_back(std::move(tu));
                }
                m_messages.push_back(std::move(assistant_msg));

                // 2. 发布本轮流式 done 事件（让 UI 知道本轮 LLM 输出结束）
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

                // 3. 执行每个 tool_use
                if (!m_tool_executor) {
                    // 未配置 executor：以错误作为 tool_result 回传
                    for (const auto& ptu : pending_tools) {
                        m_messages.push_back(ChatMessage::tool_result(
                            ptu.id, ptu.name, "Error: tool executor not configured"));
                    }
                    continue;  // 继续下一轮 agent 循环
                }

                tool::ToolContext ctx;
                ctx.cwd = std::filesystem::current_path().string();
                ctx.session_id = "default";

                for (const auto& ptu : pending_tools) {
                    nlohmann::json input_json;
                    try {
                        input_json = ptu.input_json.empty()
                            ? nlohmann::json::object()
                            : nlohmann::json::parse(ptu.input_json);
                    } catch (const nlohmann::json::parse_error&) {
                        input_json = nlohmann::json::object();
                    }

                    // UI 反馈：工具调用开始
                    EventBus::instance().publish_async(StreamTokenEvent{
                        .session_id = "default",
                        .content_delta = std::format("\n[Tool: {}]\n", ptu.name),
                        .reasoning_delta = "",
                        .is_thinking = false,
                        .token_count = 0
                    });

                    auto exec_result = m_tool_executor->execute(ptu.name, input_json, ctx);
                    std::string result_text = exec_result.result.to_string();

                    // UI 反馈：工具结果
                    EventBus::instance().publish_async(StreamTokenEvent{
                        .session_id = "default",
                        .content_delta = std::format("[Result]: {}\n", result_text),
                        .reasoning_delta = "",
                        .is_thinking = false,
                        .token_count = 0
                    });

                    // 添加 tool_result 消息（下一轮 LLM 会读取）
                    m_messages.push_back(ChatMessage::tool_result(
                        ptu.id, ptu.name, result_text));
                }

                // 继续下一轮 agent 循环（LLM 根据 tool_result 决定下一步）
            }

            // ---- 超过最大迭代数 ----
            EventBus::instance().publish_async(StreamErrorEvent{
                .session_id = "default",
                .message = std::format("Agent loop reached max iterations ({})", max_iterations),
                .retryable = false
            });
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
