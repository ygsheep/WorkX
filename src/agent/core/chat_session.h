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

/// @brief B.1：流式响应中累积的待执行 tool_use（run_completion 拆分用）
struct PendingToolUse {
    std::string id;
    std::string name;
    std::string input_json;
};

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
    /// @brief 构建推理请求（含 tools schema 和消息历史）
    CompletionRequest build_request() const;

    /// @brief 执行推理（在后台线程中运行，含 agent 循环）
    /// @param user_text 用户输入文本
    /// @param retry_attempt 当前重试次数（0=首次请求）
    void run_completion(const std::string& user_text, int retry_attempt = 0);

    // ============================================================
    // B.1：agent 循环子方法（run_completion 拆分）
    // ============================================================

    /// @brief agent 单轮迭代的后续动作
    enum class AgentStepResult {
        Continue,       ///< 继续下一轮 agent 循环（有 tool_use 已执行）
        Done,           ///< 正常完成（无 tool_use），已发布 done 事件
        RetryNewTask,   ///< 已启动递归重试新 Task，调用方应直接退出
        Cancelled,      ///< 已取消，已发布 interrupted done 事件
        ErrorExit,      ///< 错误退出（重试耗尽或异常），已发布 error 事件
    };

    /// @brief 可中断的退避等待（B.1/B.2 抽取）
    /// @return true 表示等待期间 should_cancel 变为 true
    bool interruptible_wait(const std::atomic<bool>& should_cancel,
                            std::chrono::milliseconds duration);

    /// @brief B.1：处理 submit 失败的重试逻辑
    /// @return RetryNewTask=已启动递归重试；ErrorExit=重试耗尽；Cancelled=被中断
    AgentStepResult handle_submit_failure(
        const std::string& user_text, int retry_attempt,
        int max_retries, int retry_delay_ms,
        const std::atomic<bool>& should_cancel, bool& resumed_new_task_out);

    /// @brief B.1：处理流式错误的重试逻辑
    /// @return RetryNewTask=已启动递归重试；ErrorExit=重试耗尽；Cancelled=被中断
    AgentStepResult handle_stream_error(
        const std::string& user_text, int retry_attempt,
        int max_retries, int retry_delay_ms,
        const std::atomic<bool>& should_cancel, bool& resumed_new_task_out);

    /// @brief B.1：读取流式响应并累积 content/reasoning/tool_uses
    /// @return Done=正常完成；Cancelled=被取消；ErrorExit=流错误
    AgentStepResult read_stream_response(
        IStreamReader& reader,
        const std::atomic<bool>& should_cancel,
        std::string& content_out, std::string& reasoning_out,
        StreamChunk& last_chunk_out, bool& stream_error_out,
        std::vector<PendingToolUse>& pending_tools_out);

    /// @brief B.1：执行 tool_use 列表，构建 assistant_msg 并执行工具，添加 tool_result 到 m_messages
    /// @param pending_tools 待执行的 tool_use 列表
    /// @param content 本轮 LLM 文本内容（构建 assistant_msg 用）
    /// @param reasoning 本轮 LLM 推理内容（构建 assistant_msg 用）
    /// @param last_chunk 最后一个流式 chunk（含 token 计数，发布 done 事件用）
    /// @param elapsed_ms 本轮耗时（毫秒）
    void execute_tool_uses(
        const std::vector<PendingToolUse>& pending_tools,
        const std::string& content, const std::string& reasoning,
        const StreamChunk& last_chunk, double elapsed_ms);

    /// @brief 订阅中断事件
    void subscribe_interrupt();

    /// @brief 取消中断订阅
    void unsubscribe_interrupt();

    /// @brief 解析 tool_use 的 input JSON 字符串
    /// @param json_str JSON 文本（可能为空）
    /// @return 解析后的 JSON 对象；空串或解析失败返回空 object
    static nlohmann::json parse_tool_input(const std::string& json_str);

    std::unique_ptr<ICompletionProvider> m_provider;
    std::vector<ChatMessage> m_messages;
    std::string m_system_prompt;
    std::string m_session_id;           ///< 会话标识（构造后不变，无需加锁）
    std::atomic<bool> m_generating{false};

    // 工具注册表与执行器（可选，为空时不启用 function calling）
    std::shared_ptr<tool::ToolRegistry> m_tool_registry;
    std::unique_ptr<tool::ToolExecutor> m_tool_executor;

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
