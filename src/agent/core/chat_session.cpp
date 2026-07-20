/**
 * @file chat_session.cpp
 * @brief 对话状态机实现
 * @details 编排用户输入、推理、流式事件发布、自动重试、会话持久化
 * @version 3.1.0
 * @date 2026-07
 */

#include "agent/core/chat_session.h"
#include "agent/message/types.h"
#include "agent/api/i_stream_reader.h"
#include "core/task/task_manager.h"
#include "core/config/config_manager.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <thread>
#include <fstream>

namespace agent {

ChatSession::ChatSession(std::unique_ptr<ICompletionProvider> provider,
                         int retry_delay_ms,
                         std::string session_id)
    : m_provider(std::move(provider))
    , m_session_id(std::move(session_id))
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
    if (m_tool_registry) {
        m_tool_executor = std::make_unique<tool::ToolExecutor>(m_tool_registry);
    } else {
        m_tool_executor.reset();
    }
}

CompletionRequest ChatSession::build_request() const {
    CompletionRequest request;
    request.stream = true;

    std::shared_ptr<tool::ToolRegistry> registry_copy;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        if (!m_system_prompt.empty()) {
            request.messages.push_back(ChatMessage::system(m_system_prompt));
        }
        // 拷贝消息列表，避免持锁调用 provider
        request.messages.insert(request.messages.end(), m_messages.begin(), m_messages.end());
        registry_copy = m_tool_registry;
    }

    // 注入工具 schema（启用 function calling）
    if (registry_copy && registry_copy->size() > 0) {
        request.tools = registry_copy->get_all_schemas();
    }

    return request;
}

void ChatSession::clear_history() {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_messages.clear();
}

void ChatSession::regenerate() {
    // 检查是否正在生成
    if (m_generating.load()) {
        EventBus::instance().publish_async(StreamErrorEvent{
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
        EventBus::instance().publish_async(StreamErrorEvent{
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
            bool resumed_new_task = false;  // 标记是否已启动递归重试新 Task

            try {
                // ---- agent 循环（LLM → tool_use → execute → tool_result → LLM）----
                const int max_iterations = 25;
                int iteration = 0;

                while (iteration < max_iterations) {
                    ++iteration;

                    // 每次 iteration 重建 request（messages 可能因 tool_result 增长）
                    CompletionRequest request = build_request();

                    auto reader = m_provider->submit_completion(request);
                    if (!reader) {
                        // B.1：submit 失败 → 委托给 handle_submit_failure
                        auto r = handle_submit_failure(user_text, retry_attempt,
                                                       max_retries, retry_delay_ms,
                                                       should_cancel, resumed_new_task);
                        if (r == AgentStepResult::RetryNewTask) break;
                        // Cancelled 或 ErrorExit：直接退出
                        break;
                    }

                    // ---- 流式读取（本轮 LLM 响应）----
                    std::vector<PendingToolUse> pending_tools;
                    StreamChunk chunk;
                    std::string full_content;
                    std::string full_reasoning;
                    auto start_time = std::chrono::steady_clock::now();
                    bool stream_error = false;

                    auto stream_r = read_stream_response(
                        *reader, should_cancel,
                        full_content, full_reasoning,
                        chunk, stream_error, pending_tools);

                    if (stream_r == AgentStepResult::Cancelled) {
                        // 处理取消：保存部分内容，发布 interrupted done
                        if (!full_content.empty()) {
                            std::lock_guard<std::mutex> lock(m_state_mutex);
                            m_messages.push_back(ChatMessage::assistant(full_content));
                        }
                        EventBus::instance().publish_async(StreamDoneEvent{
                            .session_id = m_session_id,
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

                    if (stream_r == AgentStepResult::ErrorExit) {
                        // B.1：流错误 → 委托给 handle_stream_error
                        auto r = handle_stream_error(user_text, retry_attempt,
                                                     max_retries, retry_delay_ms,
                                                     should_cancel, resumed_new_task);
                        if (r == AgentStepResult::RetryNewTask) break;
                        // Cancelled 或 ErrorExit：直接退出
                        break;
                    }

                    // ---- 流式完成 ----
                    auto end_time = std::chrono::steady_clock::now();
                    double elapsed_ms = std::chrono::duration<double, std::milli>(
                        end_time - start_time).count();

                    // 没有 tool_use：正常完成，发布 done 事件并退出 agent 循环
                    if (pending_tools.empty()) {
                        {
                            std::lock_guard<std::mutex> lock(m_state_mutex);
                            m_messages.push_back(ChatMessage::assistant(full_content));
                            if (!full_reasoning.empty()) {
                                m_messages.back().reasoning_content = full_reasoning;
                            }
                        }

                        EventBus::instance().publish_async(StreamDoneEvent{
                            .session_id = m_session_id,
                            .full_content = full_content,
                            .full_reasoning = full_reasoning,
                            .was_interrupted = false,
                            .prompt_tokens = chunk.prompt_tokens,
                            .generated_tokens = chunk.generated_tokens,
                            .prompt_ms = chunk.prompt_ms,
                            .generation_ms = elapsed_ms
                        });
                        break;
                    }

                    // ---- 有 tool_use：执行工具并继续 agent 循环 ----
                    execute_tool_uses(pending_tools, full_content, full_reasoning, chunk, elapsed_ms);
                    // 继续下一轮 agent 循环
                }

                // ---- 超过最大迭代数 ----
                if (iteration >= max_iterations && !resumed_new_task) {
                    EventBus::instance().publish_async(StreamErrorEvent{
                        .session_id = m_session_id,
                        .message = std::format("Agent loop reached max iterations ({})", max_iterations),
                        .retryable = false
                    });
                }
            } catch (const std::exception& e) {
                EventBus::instance().publish_async(StreamErrorEvent{
                    .session_id = m_session_id,
                    .message = std::format("Agent loop exception: {}", e.what()),
                    .retryable = false
                });
            } catch (...) {
                EventBus::instance().publish_async(StreamErrorEvent{
                    .session_id = m_session_id,
                    .message = "Agent loop unknown exception",
                    .retryable = false
                });
            }

            // completion_done 等价点：仅当未启动递归重试新 Task 时才复位
            if (!resumed_new_task) {
                m_generating.store(false);
                {
                    std::lock_guard<std::mutex> lock(m_state_mutex);
                    m_current_task.reset();
                }
            }
            m_task_cv.notify_all();
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
// B.1：拆分出的子方法实现
// ============================================================

bool ChatSession::interruptible_wait(const std::atomic<bool>& should_cancel,
                                     std::chrono::milliseconds duration) {
    auto wait_until = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < wait_until) {
        if (should_cancel) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

ChatSession::AgentStepResult ChatSession::handle_submit_failure(
    const std::string& user_text, int retry_attempt,
    int max_retries, int retry_delay_ms,
    const std::atomic<bool>& should_cancel, bool& resumed_new_task_out) {

    if (retry_attempt >= max_retries) {
        EventBus::instance().publish_async(StreamErrorEvent{
            .session_id = m_session_id,
            .message = "Failed to submit completion request after retries",
            .retryable = false
        });
        return AgentStepResult::ErrorExit;
    }

    int delay = retry_delay_ms * (1 << retry_attempt);
    EventBus::instance().publish_async(StreamErrorEvent{
        .session_id = m_session_id,
        .message = std::format("Submit failed, retrying in {}ms... ({}/{})",
                               delay, retry_attempt + 1, max_retries),
        .retryable = true
    });

    // 可中断的等待
    if (interruptible_wait(should_cancel, std::chrono::milliseconds(delay))) {
        return AgentStepResult::Cancelled;
    }

    // 递归重试：清除当前 task 引用，让新 task 接管
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_current_task.reset();
    }
    resumed_new_task_out = true;
    run_completion(user_text, retry_attempt + 1);
    return AgentStepResult::RetryNewTask;
}

ChatSession::AgentStepResult ChatSession::handle_stream_error(
    const std::string& user_text, int retry_attempt,
    int max_retries, int retry_delay_ms,
    const std::atomic<bool>& should_cancel, bool& resumed_new_task_out) {

    if (retry_attempt >= max_retries) {
        EventBus::instance().publish_async(StreamErrorEvent{
            .session_id = m_session_id,
            .message = "Stream error occurred after retries",
            .retryable = false
        });
        return AgentStepResult::ErrorExit;
    }

    int delay = retry_delay_ms * (1 << retry_attempt);
    EventBus::instance().publish_async(StreamErrorEvent{
        .session_id = m_session_id,
        .message = std::format("Stream error, retrying in {}ms... ({}/{})",
                               delay, retry_attempt + 1, max_retries),
        .retryable = true
    });

    if (interruptible_wait(should_cancel, std::chrono::milliseconds(delay))) {
        return AgentStepResult::Cancelled;
    }

    // 移除可能部分添加的 assistant 消息，然后递归重试
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        while (!m_messages.empty() &&
               m_messages.back().role == ChatMessage::Role::Assistant) {
            m_messages.pop_back();
        }
    }
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_current_task.reset();
    }
    resumed_new_task_out = true;
    run_completion(user_text, retry_attempt + 1);
    return AgentStepResult::RetryNewTask;
}

ChatSession::AgentStepResult ChatSession::read_stream_response(
    IStreamReader& reader,
    const std::atomic<bool>& should_cancel,
    std::string& content_out, std::string& reasoning_out,
    StreamChunk& last_chunk_out, bool& stream_error_out,
    std::vector<PendingToolUse>& pending_tools_out) {

    while (true) {
        if (should_cancel) {
            reader.cancel();
            return AgentStepResult::Cancelled;
        }

        StreamChunk chunk;
        auto state = reader.next(
            [&should_cancel]() { return should_cancel.load(); },
            chunk
        );

        if (state == StreamState::HasData) {
            // 文本/推理增量
            if (!chunk.content_delta.empty() || !chunk.reasoning_delta.empty()) {
                content_out += chunk.content_delta;
                reasoning_out += chunk.reasoning_delta;

                EventBus::instance().publish_async(StreamTokenEvent{
                    .session_id = m_session_id,
                    .content_delta = chunk.content_delta,
                    .reasoning_delta = chunk.reasoning_delta,
                    .is_thinking = !chunk.reasoning_delta.empty(),
                    .token_count = chunk.token_count
                });
            }

            // tool_use content_block 开始
            if (chunk.is_tool_use_start) {
                pending_tools_out.push_back({
                    chunk.tool_use_id,
                    chunk.tool_name,
                    ""
                });
            }

            // tool_use input JSON 增量（流式拼接）
            if (chunk.is_tool_use_delta && !pending_tools_out.empty()) {
                pending_tools_out.back().input_json += chunk.tool_input_delta;
            }
            last_chunk_out = chunk;
        } else if (state == StreamState::Complete) {
            if (!chunk.content_delta.empty() || !chunk.reasoning_delta.empty()) {
                content_out += chunk.content_delta;
                reasoning_out += chunk.reasoning_delta;
            }
            last_chunk_out = chunk;
            return AgentStepResult::Done;
        } else if (state == StreamState::Error) {
            stream_error_out = true;
            return AgentStepResult::ErrorExit;
        } else if (state == StreamState::Cancelled) {
            return AgentStepResult::Cancelled;
        }
    }
}

void ChatSession::execute_tool_uses(
    const std::vector<PendingToolUse>& pending_tools,
    const std::string& content, const std::string& reasoning,
    const StreamChunk& last_chunk, double elapsed_ms) {

    // 1. 构建 assistant 消息（包含 text + tool_uses）
    ChatMessage assistant_msg = ChatMessage::assistant(content);
    if (!reasoning.empty()) {
        assistant_msg.reasoning_content = reasoning;
    }
    for (const auto& ptu : pending_tools) {
        ToolUse tu;
        tu.id = ptu.id;
        tu.name = ptu.name;
        tu.input = parse_tool_input(ptu.input_json);
        assistant_msg.tool_uses.push_back(std::move(tu));
    }
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_messages.push_back(std::move(assistant_msg));
    }

    // 2. 发布本轮流式 done 事件（让 UI 知道本轮 LLM 输出结束）
    EventBus::instance().publish_async(StreamDoneEvent{
        .session_id = m_session_id,
        .full_content = content,
        .full_reasoning = reasoning,
        .was_interrupted = false,
        .prompt_tokens = last_chunk.prompt_tokens,
        .generated_tokens = last_chunk.generated_tokens,
        .prompt_ms = last_chunk.prompt_ms,
        .generation_ms = elapsed_ms
    });

    // 3. 执行每个 tool_use
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        if (!m_tool_executor) {
            // 未配置 executor：以错误作为 tool_result 回传
            for (const auto& ptu : pending_tools) {
                m_messages.push_back(ChatMessage::tool_result(
                    ptu.id, ptu.name, "Error: tool executor not configured"));
            }
            return;  // 调用方继续下一轮 agent 循环
        }
    }

    tool::ToolContext ctx;
    ctx.cwd = std::filesystem::current_path().string();
    ctx.session_id = m_session_id;

    for (const auto& ptu : pending_tools) {
        nlohmann::json input_json = parse_tool_input(ptu.input_json);

        // UI 反馈：工具调用开始
        EventBus::instance().publish_async(StreamTokenEvent{
            .session_id = m_session_id,
            .content_delta = std::format("\n[Tool: {}]\n", ptu.name),
            .reasoning_delta = "",
            .is_thinking = false,
            .token_count = 0
        });

        auto exec_result = m_tool_executor->execute(ptu.name, input_json, ctx);
        std::string result_text = exec_result.result.to_string();

        // UI 反馈：工具结果
        EventBus::instance().publish_async(StreamTokenEvent{
            .session_id = m_session_id,
            .content_delta = std::format("[Result]: {}\n", result_text),
            .reasoning_delta = "",
            .is_thinking = false,
            .token_count = 0
        });

        // 添加 tool_result 消息（下一轮 LLM 会读取）
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_messages.push_back(ChatMessage::tool_result(
            ptu.id, ptu.name, result_text));
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

// ============================================================
// parse_tool_input — 工具输入 JSON 解析（空串或解析失败返回空 object）
// ============================================================

nlohmann::json ChatSession::parse_tool_input(const std::string& json_str) {
    if (json_str.empty()) {
        return nlohmann::json::object();
    }
    try {
        return nlohmann::json::parse(json_str);
    } catch (const nlohmann::json::parse_error&) {
        return nlohmann::json::object();
    }
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
