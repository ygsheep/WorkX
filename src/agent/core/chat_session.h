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
#include "agent/api/i_completion_provider.h"
#include "agent/api/chat_types.h"
#include "core/events/event_bus.h"
#include "core/utils/result.h"
#include "agent/message/types.h"

namespace workx {

/// @brief 对话会话
/// @details 订阅 UserInputEvent/CommandEvent/InterruptEvent，
///          后台 Task 调用 ICompletionProvider，发布 StreamTokenEvent/StreamDoneEvent
class ChatSession {
public:
    /// @brief 构造
    /// @param provider 推理提供者（IBackend 或 IAgentCore）
    /// @param retry_delay_ms 重试初始延迟（毫秒），会被 backend.retry_delay_ms 覆盖
    explicit ChatSession(std::unique_ptr<ICompletionProvider> provider, int retry_delay_ms = 1000);

    ~ChatSession();

    /// @brief 添加系统提示词
    void set_system_prompt(const std::string& prompt);

    /// @brief 清空对话历史
    void clear_history();

    /// @brief 重新生成最后一条回复
    void regenerate();

    /// @brief 获取对话历史
    const std::vector<ChatMessage>& get_messages() const { return m_messages; }

    /// @brief 是否正在生成
    bool is_generating() const { return m_generating.load(); }

    /// @brief 保存对话历史到文件
    Result<void, std::string> save_session(const std::string& path) const;

    /// @brief 从文件加载对话历史
    Result<void, std::string> load_session(const std::string& path);

private:
    /// @brief 处理用户输入
    void on_user_input(const UserInputEvent& event);

    /// @brief 执行推理（在后台线程中运行）
    /// @param user_text 用户输入文本
    /// @param retry_attempt 当前重试次数（0=首次请求）
    void run_completion(const std::string& user_text, int retry_attempt = 0);

    /// @brief 订阅所有事件
    void subscribe_events();

    /// @brief 取消所有订阅
    void unsubscribe_events();

    std::unique_ptr<ICompletionProvider> m_provider;
    std::vector<ChatMessage> m_messages;
    std::string m_system_prompt;
    std::atomic<bool> m_generating{false};

    // 重试配置
    int m_max_retries = 3;
    int m_retry_delay_ms = 1000;

    // 事件订阅 token
    EventToken m_user_input_token;
    EventToken m_command_token;
    EventToken m_interrupt_token;
};

} // namespace workx
