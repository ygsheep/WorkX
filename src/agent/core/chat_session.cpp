/**
 * @file chat_session.cpp
 * @brief 对话状态机实现
 * @details 编排用户输入、ReAct 循环、流式事件发布、自动重试、会话持久化
 * @version 3.1.0
 * @date 2026-07
 */

#include "agent/core/chat_session.h"
#include "agent/core/react_loop.h"
#include "agent/message/types.h"
#include "core/task/task_manager.h"
#include "core/config/config_manager.h"

#include "agent/core/chat_session.h"
#include "agent/core/react_loop.h"
#include "agent/api/i_backend.h"
#include "agent/message/types.h"
#include "core/task/task_manager.h"
#include "core/config/config_manager.h"
#include <nlohmann/json.hpp>

#include <chrono>
#include <thread>
#include <fstream>

namespace agent {

// ============================================================
// 内部辅助
// ============================================================

namespace {

/// @brief 根据工具名推断 ToolType（用于 ToolCallEvent）
ToolType infer_tool_type(const std::string& name) {
    if (name == "Read")  return ToolType::ReadFile;
    if (name == "Write") return ToolType::WriteFile;
    if (name == "Edit")  return ToolType::EditFile;
    if (name == "Bash")  return ToolType::Execute;
    if (name == "Grep" || name == "Glob") return ToolType::Search;
    if (name == "Agent") return ToolType::Agent;
    return ToolType::Other;
}

} // anonymous namespace

// ============================================================
// ChatSession::ReActEventPublisher — 3.2 IReActObserver 实现
// ============================================================

ChatSession::ReActEventPublisher::ReActEventPublisher(IEventBus& bus, std::string session_id)
    : m_bus(bus), m_session_id(std::move(session_id)) {}

void ChatSession::ReActEventPublisher::on_thought(const ReActStep& step) {
    // 发布 AgentStepEvent
    // 注意：description 只用简短占位。流式期间 on_token 已通过
    // StreamTokenEvent 把 thought_text 完整渲染到终端，这里若再
    // 写完整文本会导致 UI 重复显示同一份内容。
    m_bus.publish_async(AgentStepEvent{
        .step_id = std::format("thought-{}", step.step_number),
        .step_number = step.step_number,
        .description = "(thinking)"
    });
    // 有 tool_use 时发布中间 StreamDoneEvent，
    // 让 UI 知道本轮 LLM 流式输出结束
    if (!step.tool_uses.empty()) {
        m_bus.publish_async(StreamDoneEvent{
            .session_id = m_session_id,
            .full_content = step.thought_text,
            .full_reasoning = step.reasoning,
            .was_interrupted = false,
            .prompt_tokens = 0,
            .generated_tokens = 0,
            .prompt_ms = 0.0,
            .generation_ms = step.duration_ms
        });
    }
}

void ChatSession::ReActEventPublisher::on_action(const ReActStep& step) {
    m_bus.publish_async(ToolCallEvent{
        .tool_name = step.tool_name,
        .arguments = step.tool_input.dump(),
        .call_id = "",
        .tool_type = infer_tool_type(step.tool_name)
    });
}

void ChatSession::ReActEventPublisher::on_observation(const ReActStep& step) {
    m_bus.publish_async(ToolResultEvent{
        .call_id = "",
        .result = step.observation,
        .is_error = step.is_error
    });
}

void ChatSession::ReActEventPublisher::on_final_answer(const ReActStep& /*step*/) {
    // 不在此发布事件，循环结束后由 run_completion 统一处理
}

void ChatSession::ReActEventPublisher::on_token(const std::string& content_delta,
                                                const std::string& reasoning_delta) {
    m_bus.publish_async(StreamTokenEvent{
        .session_id = m_session_id,
        .content_delta = content_delta,
        .reasoning_delta = reasoning_delta,
        .is_thinking = !reasoning_delta.empty(),
        .token_count = 0
    });
}

// ============================================================
// 构造与析构
// ============================================================

ChatSession::ChatSession(std::unique_ptr<ICompletionProvider> provider,
                         int retry_delay_ms,
                         std::string session_id,
                         ITaskManager& task_manager,
                         IEventBus& event_bus,
                         IConfigManager& config_manager)
    : m_provider(std::move(provider))
    , m_session_id(std::move(session_id))
    , m_task_manager(task_manager)
    , m_event_bus(event_bus)
    , m_config_manager(config_manager)
{
    // H-3：从配置管理器读取重试配置，统一委托给 HttpRetryPolicy
    // 注意：仅当配置中显式设置时才覆盖，否则使用 preset 传入的值（可为不同 provider 设置不同延迟）
    auto& cfg = m_config_manager.get();
    m_retry_policy.max_retries = cfg.has("backend.retry_count")
        ? cfg.get_or<int>("backend.retry_count", 3)
        : 3;
    m_retry_policy.base_delay_ms = cfg.has("backend.retry_delay_ms")
        ? cfg.get_or<int>("backend.retry_delay_ms", retry_delay_ms)
        : retry_delay_ms;

    subscribe_interrupt();
}

ChatSession::~ChatSession() {
    unsubscribe_interrupt();
    if (m_provider) {
        m_provider->interrupt();
    }
    // 等待后台任务完成，防止 use-after-free
    std::shared_ptr<Task> task;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        task = m_current_task;
    }
    if (task) {
        task->cancel();
        // 等待任务结束（最长 30 秒兜底）
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (task->isRunning() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

void ChatSession::set_system_prompt(const std::string& prompt) {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_system_prompt = prompt;
}

void ChatSession::set_tool_registry(std::shared_ptr<tool::ToolRegistry> registry) {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_tool_registry = std::move(registry);
    // executor 由 ReActLoop 内部创建，此处仅保存 registry
}

void ChatSession::clear_history() {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_messages.clear();
}

void ChatSession::regenerate() {
    // 检查是否正在生成
    if (m_generating.load()) {
        m_event_bus.get().publish_async(StreamErrorEvent{
            .session_id = m_session_id,
            .message = "Still generating, cannot regenerate",
            .retryable = true
        });
        return;
    }

    std::string last_user_text;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        while (!m_messages.empty() &&
               m_messages.back().role == ChatMessage::Role::Assistant) {
            m_messages.pop_back();
        }
        if (!m_messages.empty() &&
            m_messages.back().role == ChatMessage::Role::User) {
            last_user_text = m_messages.back().content;
            m_messages.pop_back();
        } else {
            return;  // 没有 user 消息可重生成
        }
    }
    run_completion(last_user_text);
}

void ChatSession::send_message(const std::string& text) {
    if (m_generating.load()) {
        m_event_bus.get().publish_async(StreamErrorEvent{
            .session_id = m_session_id,
            .message = "Still generating, please wait or press Ctrl+C to interrupt",
            .retryable = true
        });
        return;
    }

    run_completion(text);
}

std::vector<ChatMessage> ChatSession::get_messages() const {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    return m_messages;
}

void ChatSession::run_completion(const std::string& user_text, int retry_attempt) {
    // B.1：拆分后的 run_completion 仅做顶层调度，agent 循环逻辑分发到子方法
    // 子方法返回 AgentStepResult，决定下一步动作（避免 goto 跨变量声明）

    // 仅首次请求时添加用户消息（重试时不重复添加）
    if (retry_attempt == 0) {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_messages.push_back(ChatMessage::user(user_text));
    }

    m_event_bus.get().publish_async(BackendStatusEvent{
        .status = BackendStatusEvent::Connecting,
        .backend_name = "session",
        .error = {}
    });

    m_generating.store(true);

    // H-3：拷贝 HttpRetryPolicy 到任务闭包（不可变结构体，线程安全）
    HttpRetryPolicy retry_policy = m_retry_policy;

    auto task = m_task_manager.get().launch("completion",
        [this, retry_attempt, retry_policy, user_text]
        (const std::atomic<bool>& should_cancel) {
            // 顶层异常安全网：确保任何未捕获异常都能重置 m_generating 并通知 UI
            try {
            // ---- 构建 tools_schema ----
            nlohmann::json tools_schema = nlohmann::json::array();
            if (m_tool_registry && m_tool_registry->size() > 0) {
                tools_schema = m_tool_registry->get_all_schemas();
            }

            // ---- 创建 ReActLoop ----
            // D-5：注入 IConfigManager，工具通过 ToolContext.config_manager() 访问
            ReActLoop loop(m_provider.get(), m_tool_registry, ReActLoop::Config{},
                           &m_config_manager.get());

            // 3.2：使用 IReActObserver 接口替代 lambda 回调
            // ReActEventPublisher 内部完成 ReActStep → IEventBus 事件转换
            ReActEventPublisher publisher(m_event_bus, m_session_id);

            // ---- 执行 ReAct 循环 ----
            ReActResult react_result = loop.run(
                m_messages, m_system_prompt, tools_schema,
                should_cancel, &publisher
            );

            // ============================================================
            // 结果处理
            // ============================================================

            // ---- 用户中断 ----
            if (react_result.was_interrupted) {
                if (!react_result.partial_content.empty()) {
                    m_messages.push_back(ChatMessage::assistant(react_result.partial_content));
                    if (!react_result.partial_reasoning.empty()) {
                        m_messages.back().reasoning_content = react_result.partial_reasoning;
                    }
                }
                m_event_bus.get().publish_async(StreamDoneEvent{
                    .session_id = m_session_id,
                    .full_content = react_result.partial_content,
                    .full_reasoning = react_result.partial_reasoning,
                    .was_interrupted = true,
                    .prompt_tokens = react_result.prompt_tokens,
                    .generated_tokens = react_result.generated_tokens,
                    .cache_creation_input_tokens = react_result.cache_creation_input_tokens,
                    .cache_read_input_tokens = react_result.cache_read_input_tokens,
                    .prompt_ms = react_result.prompt_ms,
                    .generation_ms = react_result.generation_ms
                });
                m_generating.store(false);
                return;
            }

            // ---- 错误处理 ----
            if (react_result.was_error) {
                // H-3：用 HttpRetryPolicy.is_retryable 统一判断
                // http_status=0 表示业务错误（非 HTTP），由 error_message 内容判断
                bool can_retry = retry_attempt < retry_policy.max_retries
                                 && HttpRetryPolicy::is_retryable(0, react_result.error_message);

                if (can_retry) {
                    // H-3：委托给 HttpRetryPolicy.delay 计算退避（含 60s 上限）
                    int delay = retry_policy.delay_ms(retry_attempt);
                    m_event_bus.get().publish_async(StreamErrorEvent{
                        .session_id = m_session_id,
                        .message = std::format(
                            "Error: {}, retrying in {}ms... ({}/{})",
                            react_result.error_message, delay,
                            retry_attempt + 1, retry_policy.max_retries),
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
                        // 保留工具调用上下文：不删除已成功的 tool call/result 消息
                        // ReAct loop 在流式失败时不会添加 partial assistant 消息，
                        // 所以 m_messages 中只有成功的 round，直接重试即可
                        run_completion(user_text, retry_attempt + 1);
                    }
                } else {
                    m_event_bus.get().publish_async(StreamErrorEvent{
                        .session_id = m_session_id,
                        .message = react_result.error_message,
                        .retryable = false
                    });
                }
                m_generating.store(false);
                return;
            }

            // ---- 成功完成 ----
            m_event_bus.get().publish_async(StreamDoneEvent{
                .session_id = m_session_id,
                .full_content = react_result.final_answer,
                .full_reasoning = react_result.final_reasoning,
                .was_interrupted = false,
                .prompt_tokens = react_result.prompt_tokens,
                .generated_tokens = react_result.generated_tokens,
                .cache_creation_input_tokens = react_result.cache_creation_input_tokens,
                .cache_read_input_tokens = react_result.cache_read_input_tokens,
                .prompt_ms = react_result.prompt_ms,
                .generation_ms = react_result.generation_ms
            });

            m_event_bus.get().publish_async(AgentDoneEvent{
                .final_response = react_result.final_answer,
                .total_steps = static_cast<int32_t>(react_result.steps.size()),
                .total_tool_calls = react_result.total_tool_calls,
                .total_duration_ms = react_result.total_duration_ms
            });

            m_generating.store(false);

            } // end try
            catch (const std::exception& e) {
                m_event_bus.get().publish_async(StreamErrorEvent{
                    .session_id = m_session_id,
                    .message = std::format("Fatal error in completion task: {}", e.what()),
                    .retryable = false
                });
                m_generating.store(false);
            } catch (...) {
                m_event_bus.get().publish_async(StreamErrorEvent{
                    .session_id = m_session_id,
                    .message = "Fatal unknown error in completion task",
                    .retryable = false
                });
                m_generating.store(false);
            }
        },
        TaskType::Normal
    );

    // 跟踪当前后台任务，用于析构等待
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_current_task = task;
    }
}

// ============================================================
// 会话持久化
// ============================================================

Result<void, std::string> ChatSession::save_session(const std::string& path) const {
    nlohmann::json j;

    // 拷贝 system_prompt 和 messages，避免长时间持锁
    std::string system_prompt_copy;
    std::vector<ChatMessage> messages_copy;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        system_prompt_copy = m_system_prompt;
        messages_copy = m_messages;
    }

    if (!system_prompt_copy.empty()) {
        j["system_prompt"] = system_prompt_copy;
    }

    auto& messages = j["messages"];
    for (const auto& msg : messages_copy) {
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
}

Result<void, std::string> ChatSession::load_session(const std::string& path) {
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

        std::vector<ChatMessage> new_messages;

        std::string new_system_prompt;
        if (j.contains("system_prompt")) {
            new_system_prompt = j["system_prompt"].get<std::string>();
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
                new_messages.push_back(std::move(msg));
            }
        }

        // 一次性加锁写入
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            m_messages = std::move(new_messages);
            m_system_prompt = std::move(new_system_prompt);
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
}

IBackend* ChatSession::backend() const {
    // L-1：通过 dynamic_cast 安全获取 IBackend 指针
    // 若 provider 不是 IBackend（如本地推理核心），返回 nullptr
    return dynamic_cast<IBackend*>(m_provider.get());
}

// ============================================================
// 事件订阅
// ============================================================

void ChatSession::subscribe_interrupt() {
    m_interrupt_token = m_event_bus.get().subscribe<InterruptEvent>(
        [this](const InterruptEvent& /*e*/) {
            if (m_provider) {
                m_provider->interrupt();
            }
        }
    );
}

void ChatSession::unsubscribe_interrupt() {
    m_event_bus.get().unsubscribe<InterruptEvent>(m_interrupt_token);
}

} // namespace agent

