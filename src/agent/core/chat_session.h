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
#include <functional>
#include <utility>   // C-1：std::pair
#include "agent/api/i_completion_provider.h"
#include "agent/api/chat_types.h"
#include "agent/api/retry.h"  // H-3：HttpRetryPolicy
#include "agent/core/react_observer.h"
#include "core/events/event_bus.h"
#include "core/config/config_manager.h"
#include "core/utils/result.h"
#include "agent/message/types.h"
#include "agent/tool/registry.h"
#include "agent/tool/executor.h"
#include "core/task/task_manager.h"

namespace agent {

/// @brief 后端接口前向声明（供 ChatSession::backend() 返回类型使用）
class IBackend;

/// @brief H-7：重试决策动作
/// @details compute_retry() 纯函数的返回类型，描述下一步动作
enum class RetryAction {
    Continue,   ///< 不重试，继续执行（无错误或不可重试错误）
    Stop,       ///< 不可重试错误，发布终止事件
    Sleep       ///< 可重试，sleep delay_ms 后递归调用 run_completion
};

/// @brief H-7：重试决策
/// @details 纯函数 compute_retry() 的返回值，分离纯逻辑与 I/O 副作用
struct RetryDecision {
    RetryAction action = RetryAction::Stop;  ///< 决策动作
    int delay_ms = 0;                        ///< Sleep 时的退避时长（毫秒）
};

/// @brief 对话会话
/// @details 由外部驱动（main.cpp），通过 send_message() 提交文本，
///          后台 Task 调用 ICompletionProvider，发布 StreamTokenEvent/StepDoneEvent/StreamDoneEvent。
///          支持工具调用（function calling）：LLM 返回 tool_use → 执行工具 → tool_result → 继续推理
class ChatSession {
public:
    /// @brief ReAct 循环事件发布器（3.2 IReActObserver 实现）
    /// @details 将 ReActLoop 步骤事件转换为 IEventBus 异步事件，
    ///          使 ReActLoop 可脱离 EventBus 体系独立使用与测试。
    class ReActEventPublisher : public IReActObserver {
    public:
        explicit ReActEventPublisher(IEventBus& bus, std::string session_id);
        ~ReActEventPublisher() override = default;

        void on_thought(const ReActStep& step) override;
        void on_action(const ReActStep& step) override;
        void on_observation(const ReActStep& step) override;
        void on_final_answer(const ReActStep& step) override;
        void on_token(const std::string& content_delta,
                      const std::string& reasoning_delta) override;

    private:
        IEventBus& m_bus;
        std::string m_session_id;
    };
    /// @brief 构造
    /// @param provider 推理提供者（IBackend 或 IAgentCore）
    /// @param task_manager 任务管理器（H-4：DI 必须显式注入，无默认实参回退单例）
    /// @param event_bus 事件总线（H-4：DI 必须显式注入，无默认实参回退单例）
    /// @param config_manager 配置管理器（H-4：DI 必须显式注入，无默认实参回退单例）
    /// @param retry_delay_ms 重试初始延迟（毫秒），会被 backend.retry_delay_ms 覆盖
    /// @param session_id 会话标识（用于事件流区分多会话，默认 "default"）
    explicit ChatSession(std::unique_ptr<ICompletionProvider> provider,
                         ITaskManager& task_manager,
                         IEventBus& event_bus,
                         IConfigManager& config_manager,
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

    /// @brief C-2：暴露 completion provider 原始指针（用于 factory 获取 IBackendAdmin）
    /// @details 调用方（factory）可通过 dynamic_cast<IBackendAdmin*> 安全转型。
    ///          返回的指针生命周期由 ChatSession 管理，session 析构后禁止使用。
    ICompletionProvider* completion_provider() const { return m_provider.get(); }

    /// @brief 是否正在生成
    bool is_generating() const { return m_generating.load(); }

    /// @brief 提交用户消息，触发 LLM 推理
    void send_message(const std::string& text);

    /// @brief 保存对话历史到文件
    /// @details H-6：仅做 serialize_state() → ofstream，序列化逻辑在 serialize_state() 中
    Result<void, std::string> save_session(const std::string& path) const;

    /// @brief 从文件加载对话历史
    /// @details H-6：仅做 ifstream → deserialize_state()，反序列化逻辑在 deserialize_state() 中
    Result<void, std::string> load_session(const std::string& path);

    /// @brief H-6：序列化会话状态为 JSON（纯函数，不触碰文件系统）
    /// @details 拷贝 system_prompt 与 messages 后构建 JSON，调用方无需加锁。
    ///          公开为 public 以便单元测试直接验证序列化格式。
    /// @return 包含 system_prompt（可选）和 messages 数组的 JSON 对象
    nlohmann::json serialize_state() const;

    /// @brief H-6：从 JSON 反序列化会话状态（C-1：真正纯函数，不触碰成员状态）
    /// @details 解析 system_prompt 与 messages，校验字段完整性。
    ///          仅对输入 j 进行只读解析，返回新构造的 (messages, system_prompt)，
    ///          调用方负责通过 commit_state() 提交到成员。
    ///          公开为 public 以便单元测试直接验证反序列化逻辑。
    /// @param j JSON 对象（来自 load_session 读取的文件）
    /// @return 成功返回 (messages, system_prompt)；失败返回错误信息
    static Result<std::pair<std::vector<ChatMessage>, std::string>, std::string>
    deserialize_state(const nlohmann::json& j);

    /// @brief C-1：提交反序列化结果到成员状态（加锁一次性写入）
    /// @details 将 deserialize_state 返回的 (messages, system_prompt) 原子提交。
    ///          load_session 内部调用；测试中也可直接构造 pair 后调用此方法
    ///          设置初始状态，避免依赖 deserialize_state 形成闭环测试。
    void commit_state(std::vector<ChatMessage> messages, std::string system_prompt);

    /// @brief H-7：纯函数计算重试决策
    /// @details 分离 5 类关注点中的纯逻辑部分：
    ///          ①可重试判定 ②退避延迟计算。
    ///          run_completion 仅按决策执行 I/O（事件发布/sleep/递归调用）。
    ///          公开为 public 以便单元测试直接验证决策。
    /// @param react_result ReAct 循环结果（只读 was_error / error_message）
    /// @param retry_policy HTTP 重试策略（提供 max_retries / is_retryable / delay_ms）
    /// @param attempt 当前重试次数（0=首次请求失败后的第一次重试判定）
    /// @return RetryDecision：Continue（无错误）/ Sleep（可重试）/ Stop（不可重试或超上限）
    /// C-3：移除 noexcept —— delay_ms 不再 noexcept，且调用链含 std::format 可能抛
    static RetryDecision compute_retry(const ReActResult& react_result,
                                       const HttpRetryPolicy& retry_policy,
                                       int attempt);

    // H-8：移除 backend() 方法。
    // 原设计暴露完整 IBackend* 给 UI 层，违反接口隔离（UI 可误调 shutdown() 等
    // IBackendAdmin 方法）。UI 层应通过 factory 独立注入的 IBackendAdmin* 调用
    // list_models / set_model_name 等管理方法。

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
    std::string m_cwd;                  ///< 会话启动时的工作目录（构造时捕获，注入到 ReActLoop）
    std::atomic<bool> m_generating{false};

    // D-1：任务管理器引用（非拥有，构造注入；ChatSession 不可移动，引用安全）
    std::reference_wrapper<ITaskManager> m_task_manager;

    // D-1：事件总线引用（非拥有，构造注入）
    std::reference_wrapper<IEventBus> m_event_bus;

    // C-1：配置管理器引用（非拥有，构造注入）
    std::reference_wrapper<IConfigManager> m_config_manager;

    // 工具注册表（可选，为空时不启用 function calling）
    std::shared_ptr<tool::ToolRegistry> m_tool_registry;

    // H-3：重试策略统一由 HttpRetryPolicy 管理
    HttpRetryPolicy m_retry_policy;

    // 中断事件订阅
    EventToken m_interrupt_token;

    // 并发控制：保护 m_messages / m_system_prompt / m_tool_registry / m_current_task
    mutable std::mutex m_state_mutex;
    std::shared_ptr<Task> m_current_task;  // 跟踪当前后台任务，用于析构等待
    std::condition_variable m_task_cv;
};

} // namespace agent
