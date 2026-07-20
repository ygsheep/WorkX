/**
 * @file chat_session.h
 * @brief 对话状态机
 * @details 持有 ICompletionProvider，处理用户事件，后台推理，发布流式事件
 * @version 3.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include "agent/api/i_completion_provider.h"
#include "agent/api/chat_types.h"
#include "core/events/event_bus.h"
#include "core/utils/result.h"
#include "agent/message/types.h"
#include "agent/tool/registry.h"
#include "agent/tool/executor.h"
#include "core/task/task_manager.h"

namespace agent {

/// @brief 对话会话
/// @details 由外部驱动（main.cpp），通过 send_message() 提交文本，
///          后台 Task 调用 ICompletionProvider，发布 StreamTokenEvent/StreamDoneEvent。
///          支持工具调用（function calling）：LLM 返回 tool_use → 执行工具 → tool_result → 继续推理
class ChatSession {
public:
    /// @brief 构造
    /// @param provider 推理提供者（IBackend 或 IAgentCore）
    /// @param retry_delay_ms 重试初始延迟（毫秒），会被 backend.retry_delay_ms 覆盖
    /// @param session_id 会话标识（用于事件流区分多会话，默认 "default"）
    explicit ChatSession(std::unique_ptr<ICompletionProvider> provider,
                         int retry_delay_ms = 1000,
                         std::string session_id = "default");

    ~ChatSession();

    /// @brief 添加系统提示词
    void set_system_prompt(const std::string& prompt);

    /// @brief 设置工具注册表（启用 function calling）
    /// @param registry 工具注册表（含已注册的工具实例）
    void set_tool_registry(std::shared_ptr<tool::ToolRegistry> registry);

    /// @brief 清空对话历史
    void clear_history();

    /// @brief 重新生成最后一条回复
    void regenerate();

    /// @brief 获取对话历史（返回拷贝，线程安全）
    std::vector<ChatMessage> get_messages() const;

    /// @brief 获取会话 ID
    const std::string& session_id() const { return m_session_id; }

    /// @brief 是否正在生成
    bool is_generating() const { return m_generating.load(); }

    /// @brief 提交用户消息，触发 LLM 推理
    void send_message(const std::string& text);

    /// @brief 保存对话历史到文件
    Result<void, std::string> save_session(const std::string& path) const;

    /// @brief 从文件加载对话历史
    Result<void, std::string> load_session(const std::string& path);

private:
    /// @brief 执行推理（在后台线程中运行，含 agent 循环）
    /// @param user_text 用户输入文本
    /// @param retry_attempt 当前重试次数（0=首次请求）
    void run_completion(const std::string& user_text, int retry_attempt = 0);

    /// @brief 订阅中断事件
    void subscribe_interrupt();

    /// @brief 取消中断订阅
    void unsubscribe_interrupt();

    std::unique_ptr<ICompletionProvider> m_provider;
    std::vector<ChatMessage> m_messages;
    std::string m_system_prompt;
    std::string m_session_id;           ///< 会话标识（构造后不变，无需加锁）
    std::atomic<bool> m_generating{false};

    // 工具注册表（可选，为空时不启用 function calling）
    std::shared_ptr<tool::ToolRegistry> m_tool_registry;

    // 重试配置
    int m_max_retries = 3;
    int m_retry_delay_ms = 1000;

    // 中断事件订阅
    EventToken m_interrupt_token;

    // 并发控制：保护 m_messages / m_system_prompt / m_tool_registry / m_current_task
    mutable std::mutex m_state_mutex;
    std::shared_ptr<Task> m_current_task;  // 跟踪当前后台任务，用于析构等待
    std::condition_variable m_task_cv;
};

} // namespace agent
