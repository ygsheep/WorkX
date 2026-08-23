/**
 * @file chat_session.h
 * @brief 对话状态机
 * @details 持有 ICompletionProvider，处理用户事件，后台推理，发布流式事件
 * @version 3.0.0
 * @date 2026-07
 */

#pragma once

#include <format>
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <unordered_set>
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
#include "agent/tool/context.h"  // #45：PermissionMode（会话级三态权限模式）
#include "agent/compact/prefix_shape.h"  // DS_CACHE: 前缀形状追踪
#include "agent/compact/cache_aware_compactor.h"  // DS_CACHE H-3: 跨 turn 持久化的压缩器
#include "agent/session/session_store.h"  // 项目会话恢复：JSONL 持久化
#include "agent/skill/inclaude/conditional.h"  // conditional skills：TouchCollector（按值成员）
#include "core/task/task_manager.h"

namespace agent {

// 前向声明（conditional skills 支持）
namespace command { class CommandRegistry; }

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
class WORKX_API ChatSession {
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

    /// @brief 获取系统提示词（返回拷贝，线程安全）
    std::string system_prompt() const;

    /// @brief 设置工具注册表（启用 function calling）
    /// @param registry 工具注册表（含已注册的工具实例）
    void set_tool_registry(std::shared_ptr<tool::ToolRegistry> registry);

    /// @brief 获取工具注册表（返回拷贝，线程安全）
    std::shared_ptr<tool::ToolRegistry> tool_registry() const;

    /// @brief 设置宿主文件索引失效回调（可选）
    /// @details FileWriteTool 写入文件后调用（如 TUI @ 补全索引 mark_dirty）。
    ///          必须在首次 send_message 前调用；不设置则工具写文件不通知宿主。
    /// @param invalidator 宿主回调（空 = 取消注册）
    void set_file_index_invalidator(std::function<void()> invalidator);

    /// @brief 设置命令注册表（conditional skills 激活匹配用）
    /// @param registry 命令注册表（含磁盘 skill 命令）
    void set_command_registry(std::shared_ptr<command::CommandRegistry> registry);

    /// @brief 获取命令注册表（返回拷贝，线程安全）
    std::shared_ptr<command::CommandRegistry> command_registry() const;

    /// @brief 获取 touch 收集器（conditional skills 用）
    /// @return TouchCollector 引用（会话内有效）
    skill::TouchCollector& touch_collector();

    /// @brief 清空对话历史
    void clear_history();

    /// @brief 重新生成最后一条回复
    void regenerate();

    /// @brief 从指定用户消息重新生成（重试按钮：截断到该用户消息后重新推理）
    /// @param user_text 触发该回复的用户消息文本（从后往前匹配最后一条相同文本）
    /// @details 与 regenerate() 的区别：regenerate() 只重生成最后一条回复；
    ///          本方法先截断到匹配的用户消息（含）之后的所有消息，再以该用户
    ///          消息重新生成，支持对历史任意一条回复做重试。
    void regenerate_from(const std::string& user_text);

    /// @brief 获取对话历史（返回拷贝，线程安全）
    std::vector<ChatMessage> get_messages() const;

    /// @brief DS_CACHE H-4：配置压缩器上下文窗口（从 provider preset 注入）
    /// @details 必须在首次 send_message 前调用。影响压缩水位判定。
    void set_compactor_context_window(int32_t context_window_tokens);

    /// @brief DS_CACHE M-1：配置压缩器归档目录（compact 折叠前归档原消息）
    /// @details 必须在首次 send_message 前调用。非空时折叠的中段消息会被
    ///          序列化到 <archive_dir>/<timestamp>.jsonl，保证可追溯。
    void set_compactor_archive_dir(const std::string& dir);

    /// @brief 设置 SessionStore（可选，设置后每条消息实时持久化到 JSONL）
    /// @details 必须在首次 send_message 前调用。设置后：
    ///          - user 消息 push_back 后立即 append 到 JSONL
    ///          - run() 返回后批量 append 新增的 assistant/tool 消息
    ///          - restore_from_file 加载的历史不会重复持久化
    void set_session_store(std::shared_ptr<agent::session::SessionStore> store);

    /// @brief 配置懒创建 SessionStore 的参数（首条 user 消息时才创建文件）
    /// @details factory 调用此方法传入配置，不立即创建文件。
    ///          首条 user 消息持久化时才创建 SessionStore + 写 session_start + title。
    /// @param project_dir 项目会话目录（<config_dir>/projects/<编码路径>）
    /// @param cwd 当前工作目录
    /// @param model 模型名
    /// @param git_branch git 分支
    void configure_session_store(const std::string& project_dir,
                                  const std::string& cwd,
                                  const std::string& model,
                                  const std::string& git_branch);

    /// @brief 从 JSONL 文件加载历史会话消息
    /// @param file_path JSONL 文件路径
    /// @return true=加载成功（至少有一条消息）
    /// @details 加载后消息追加到 m_messages（不清空已有消息）。
    ///          加载的消息不会触发持久化（避免回环）。
    bool restore_from_file(const std::string& file_path);

    /// @brief 新建会话（/new 与 /clear 共用）：清空消息并切换到新 session_id
    /// @details 保留旧会话文件（/new 语义）；/clear 由调用方在切换后删除旧文件。
    ///          生成中安全（先取消并等待当前任务）：
    ///          1. 生成新 session_id（core::util::generate_uuid）
    ///          2. 清空 m_messages
    ///          3. 关闭旧 SessionStore 并置空（新会话文件懒创建：首条 user 消息时
    ///             以新 session_id 创建 JSONL，复用启动时 configure_session_store 参数）
    ///          4. 重置压缩器与前缀形状基线 + conditional skills 会话级累积
    ///          锁外重置 TodoStore 到新会话空清单（restore_todos 内部再加 m_state_mutex）。
    void new_session();

    /// @brief 切换到历史会话（/resume 命令调用）
    /// @param file_path 历史会话 JSONL 文件路径
    /// @return true=切换成功
    /// @details 原子操作（单一锁作用域）：
    ///          1. load_messages + load_meta 加载历史
    ///          2. 替换 m_session_id（从文件名 stem 提取）
    ///          3. 清空 m_messages，填入历史消息
    ///          4. 关闭旧 SessionStore，创建新 SessionStore 指向历史文件，append 模式打开
    ///          5. 不追加 session_start（会话进行中，只是换文件继续写）
    ///          6. 重置压缩器和前缀形状基线
    ///          不写 session_end 到旧文件（会话可被多次 resume 继续）。
    bool switch_session(const std::string& file_path);

    /// @brief 从内存导入消息历史（/provider 热切换后保留当前对话继续）
    /// @param messages 待导入的消息（通常来自旧 session 的 get_messages）
    /// @details 清空现有消息后填入，并重置压缩器与前缀形状基线（与 switch_session
    ///          保持一致），不涉及文件 I/O、不改变 SessionStore。
    void import_messages(std::vector<ChatMessage> messages);

    /// @brief 修改当前会话标题（/rename 命令调用）
    /// @param title 新标题
    /// @return true=成功追加 title 事件
    /// @details append-only：追加新 title 事件到当前 JSONL，读取时取最后一条。
    bool rename_session(const std::string& title);

    /// @brief 获取 SessionStore（用于退出时写入 session_end）
    std::shared_ptr<agent::session::SessionStore> session_store() const { return m_session_store; }

    /// @brief 获取会话 ID
    const std::string& session_id() const { return m_session_id; }

    /// @brief C-2：暴露 completion provider 原始指针（用于 factory 获取 IBackendAdmin）
    /// @details 调用方（factory）可通过 dynamic_cast<IBackendAdmin*> 安全转型。
    ///          返回的指针生命周期由 ChatSession 管理，session 析构后禁止使用。
    ICompletionProvider* completion_provider() const { return m_provider.get(); }

    /// @brief 运行时切换推理后端（/provider 热切换）
    /// @param provider 新后端（须已 initialize；空指针忽略）
    /// @return true=已替换；false=生成中拒绝切换（调用方应先用 is_generating 检查）
    /// @details 线程安全（受 m_state_mutex 保护）。ReActLoop 每次 run 时从
    ///          m_provider 取指针（chat_session.cpp run_completion 内新建），
    ///          非生成中替换无并发访问。切换后调用方通常需 import_messages
    ///          保留对话继续（消息在 ChatSession 内，不受 provider 影响）。
    bool set_provider(std::unique_ptr<ICompletionProvider> provider);

    /// @brief #45：设置会话级权限模式（CLI --bypass-permissions 注入）
    /// @details 仅接受 Default/BypassPermissions（Plan 由 toggle_permission_mode 管理）。
    ///          线程安全（受 m_state_mutex 保护）。跨 turn 生效（下一轮 ReActLoop 注入）。
    void set_permission_mode(tool::PermissionMode mode);

    /// @brief #45：获取当前权限模式（线程安全）
    tool::PermissionMode permission_mode() const;

    /// @brief #45：三态切换权限模式（Default → Plan → Bypass → Default）
    /// @details Shift+Tab 触发。进入 Plan 时记录 before_plan（ExitPlanMode 工具
    ///          退出恢复用）。线程安全（受 m_state_mutex 保护）。
    void toggle_permission_mode();

    /// @brief 是否正在生成
    bool is_generating() const { return m_generating.load(); }

    /// @brief 暂停模型活动：取消并等待当前后台任务完全退出
    /// @details 供宿主（TUI /edit 等）在需要独占文件/状态时调用，避免后台
    ///          ReActLoop 写文件与宿主操作冲突。线程安全（内部持锁）。
    void cancel_current_task() { cancel_and_wait_current_task(); }

    /// @brief 提交用户消息，触发 LLM 推理
    /// @param text 用户文本
    /// @param images 图片附件绝对路径（多模态，可为空）
    void send_message(const std::string& text, const std::vector<std::string>& images = {});

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
    /// @param images 图片附件绝对路径（仅首次请求使用，重试沿用已入列的图片消息）
    /// @param retry_attempt 当前重试次数（0=首次请求）
    void run_completion(const std::string& user_text,
                        const std::vector<std::string>& images = {},
                        int retry_attempt = 0);

    /// @brief 订阅中断事件
    void subscribe_interrupt();

    /// @brief 取消中断订阅
    void unsubscribe_interrupt();

    /// @brief 订阅子 Agent 进度/完成事件并持久化到 SessionStore
    /// @details 第二层（子 Agent 记录）持久化：SubAgentProgressEvent/SubAgentCompletedEvent
    ///          发布时追加 sub_agent 事件到当前 SessionStore，/resume 时按序重放恢复。
    void subscribe_sub_agent_persistence();

    /// @brief 取消子 Agent 事件持久化订阅
    void unsubscribe_sub_agent_persistence();

    /// @brief DS_CACHE M-4：LLM 摘要回调（注入到 m_compactor）
    /// @param middle 待摘要的中段消息序列
    /// @return LLM 生成的摘要文本（失败时抛异常，由 compact_middle fallback 到机械折叠）
    /// @details 同步调用 m_provider 完成摘要生成：
    ///          1. 构造摘要 system prompt + middle 消息
    ///          2. submit_completion 提交
    ///          3. 阻塞 next() 读取流直到 Complete，拼接 content
    ///          4. 失败（nullptr/Error/Cancelled）抛异常触发 fallback
    std::string summarize_with_llm(const std::vector<ChatMessage>& middle);

    /// @brief 持久化单条消息到 SessionStore（如果已设置）
    /// @details 根据 msg.role 调用对应的 append 方法。
    ///          uuid 由 core::util::generate_uuid() 生成，timestamp 由 now_iso() 生成。
    void persist_message(const ChatMessage& msg);

    /// @brief 批量持久化 [start_idx, end) 范围的消息
    /// @param start_idx 起始索引（含）
    /// @param parent_uuid 父消息 UUID（用于 parentUuid 字段）
    void persist_messages_range(size_t start_idx, const std::string& parent_uuid = "");

    /// @brief 生成中安全：取消并等待当前后台任务完全退出后，再复位生成标志
    /// @details 切换/清理会话共享状态（m_messages / m_compactor / m_session_store）前必须先调用。
    ///          因为 ReActLoop::run 通过非 const 引用直接读写 m_messages，若调用方（UI 线程）
    ///          并发执行 switch_session/clear_history/import_messages 对 m_messages 做 move/clear，
    ///          会与任务线程形成数据竞争 → 堆损坏（resume 后重发消息崩溃的 UAF 根因）。
    ///          复刻析构 wait 模式：cancel 当前任务 + wait → cancelAll + waitForAll → 复位标志。
    void cancel_and_wait_current_task();
    void cancel_background_tasks();  // 定向取消本会话已分发的后台任务（P1-1）

    /// @brief #24：接线 TodoStore 持久化回调（session_id → 当前 SessionStore 写 todo 事件）
    /// @details 在 SessionStore 创建/切换后调用（懒创建 + switch_session）。
    ///          回调捕获 store 的 shared_ptr，TodoStore 每次变更时追加全量快照。
    void wire_todo_persistence();

    std::unique_ptr<ICompletionProvider> m_provider;
    std::vector<ChatMessage> m_messages;
    std::string m_system_prompt;
    std::string m_session_id;           ///< 会话标识（switch_session 可变更，由 m_state_mutex 保护）
    std::string m_cwd;                  ///< 会话启动时的工作目录（构造时捕获，注入到 ReActLoop）

    // #45：会话级权限模式（三态：Default/Plan/Bypass），由 m_state_mutex 保护
    tool::PermissionMode m_permission_mode{tool::PermissionMode::Default};
    ///< 进入 Plan 前的原模式（Plan 退出恢复用，由 m_state_mutex 保护）
    tool::PermissionMode m_permission_mode_before_plan{tool::PermissionMode::Default};
    std::atomic<bool> m_generating{false};

    // DS_CACHE M-3：移除 m_cache_hit_total/m_cache_miss_total 死代码
    // （TUI 使用 token_stats_model.h 自有的累计器，ChatSession 侧为死代码）

    // 上一轮前缀形状（用于本轮对比，诊断缓存劣化归因）
    // 由 m_state_mutex 保护（与 system_prompt / tools 同步更新）
    PrefixShape m_last_prefix_shape;

    // DS_CACHE H-3：跨 turn 持久化的压缩器（卡死守卫/rewrite_version 跨 turn 累积）
    // 由 m_state_mutex 保护（与 messages 生命周期同步）
    CacheAwareCompactor m_compactor;

    // D-1：任务管理器引用（非拥有，构造注入；ChatSession 不可移动，引用安全）
    std::reference_wrapper<ITaskManager> m_task_manager;

    // D-1：事件总线引用（非拥有，构造注入）
    std::reference_wrapper<IEventBus> m_event_bus;

    // C-1：配置管理器引用（非拥有，构造注入）
    std::reference_wrapper<IConfigManager> m_config_manager;

    // 工具注册表（可选，为空时不启用 function calling）
    std::shared_ptr<tool::ToolRegistry> m_tool_registry;

    // conditional skills：命令注册表（激活匹配用，可选）
    std::shared_ptr<command::CommandRegistry> m_command_registry;

    // conditional skills：touch 路径收集器（会话级累积）+ 已激活 skill 名（避免重复注入）
    skill::TouchCollector m_touch_collector;
    std::unordered_set<std::string> m_activated_skills;

    // 宿主文件索引失效回调（FileWriteTool 写文件后调用，由 m_state_mutex 保护）
    std::function<void()> m_file_index_invalidator;

    // 项目会话恢复：JSONL 持久化（可选，设置后每条消息实时追加）
    std::shared_ptr<agent::session::SessionStore> m_session_store;

    // 懒创建 SessionStore 配置（首条 user 消息时才创建文件）
    bool m_store_configured = false;
    std::string m_store_project_dir;
    std::string m_store_cwd;
    std::string m_store_model;
    std::string m_store_git_branch;

    // H-3：重试策略统一由 HttpRetryPolicy 管理
    HttpRetryPolicy m_retry_policy;

    // 中断事件订阅
    EventToken m_interrupt_token;

    // 子 Agent 事件持久化订阅（progress/completed）
    EventToken m_sub_progress_token;
    EventToken m_sub_completed_token;

    // 并发控制：保护 m_messages / m_system_prompt / m_tool_registry / m_current_task
    mutable std::mutex m_state_mutex;
    std::shared_ptr<Task> m_current_task;  // 跟踪当前后台任务，用于析构等待
    std::condition_variable m_task_cv;
    /// 本会话已分发的后台任务 id（P1-1 定向取消；受 m_state_mutex 保护）
    std::vector<std::string> m_background_task_ids;
};

} // namespace agent
