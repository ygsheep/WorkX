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
#include "agent/tool/tool_kind.h"
#include "core/task/task_manager.h"
#include "core/config/config_manager.h"
#include "core/utils/uuid.h"  // 项目会话恢复：UUID 生成

#include "agent/api/i_backend.h"
#include "agent/api/i_stream_reader.h"
#include <nlohmann/json.hpp>

#include <chrono>
#include <thread>
#include <fstream>
#include <filesystem>
#include <ctime>

namespace agent {

// ============================================================
// 内部辅助
// ============================================================

// L-1：infer_tool_type 已提升至 core/tool_kind.h/.cpp 作为公共纯函数，
//      此处不再保留匿名命名空间副本，直接使用 agent::tool::infer_tool_type。

namespace {

/// @brief 获取当前 ISO 8601 时间戳（UTC），用于会话持久化
/// @details 格式：YYYY-MM-DDTHH:MM:SSZ
std::string now_iso() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
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
    // P3：有 tool_use 时发布 StepDoneEvent（而非 StreamDoneEvent），
    // 让 UI 知道本轮 LLM 流式输出结束，但不触发会话级结束动作。
    // 原实现发布 StreamDoneEvent 导致语义污染：UI 误显示完成、
    // token 统计被 0 值覆盖、状态机错误转 IDLE、光标错位。
    if (!step.tool_uses.empty()) {
        m_bus.publish_async(StepDoneEvent{
            .session_id = m_session_id,
            .full_content = step.thought_text,
            .full_reasoning = step.reasoning,
            .generation_ms = step.duration_ms
        });
    }
}

void ChatSession::ReActEventPublisher::on_action(const ReActStep& step) {
    m_bus.publish_async(ToolCallEvent{
        .tool_name = step.tool_name,
        .arguments = step.tool_input.dump(),
        .call_id = "",
        .tool_type = tool::infer_tool_type(step.tool_name)
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
                         ITaskManager& task_manager,
                         IEventBus& event_bus,
                         IConfigManager& config_manager,
                         int retry_delay_ms,
                         std::string session_id)
    : m_provider(std::move(provider))
    , m_session_id(std::move(session_id))
    , m_cwd(std::filesystem::current_path().string())
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

    // DS_CACHE H-3：注册压缩器暂停回调，发布 CompactionPausedEvent 到 EventBus
    m_compactor.set_paused_callback(
        [this](bool paused, int consecutive_compacts, int32_t tokens, float ratio,
               const std::string& notice) {
            m_event_bus.get().publish_async(CompactionPausedEvent{
                .session_id = m_session_id,
                .paused = paused,
                .consecutive_compacts = static_cast<int32_t>(consecutive_compacts),
                .tokens_before = tokens,
                .ratio = ratio,
                .notice = notice
            });
        });

    // DS_CACHE M-4：注入 LLM 摘要回调（compact 阶段调用，失败自动 fallback 到机械折叠）
    // 捕获 this 以访问 m_provider；provider 生命周期与 ChatSession 绑定，安全。
    m_compactor.set_summarize_fn(
        [this](const std::vector<ChatMessage>& middle) {
            return this->summarize_with_llm(middle);
        });

    subscribe_interrupt();
}

ChatSession::~ChatSession() {
    unsubscribe_interrupt();
    if (m_provider) {
        m_provider->interrupt();
    }
    // H-9：等待后台任务完成，防止 use-after-free
    // 改用 ITaskManager::wait(task) 替代 sleep_for(50ms) 轮询
    // wait 内部用 condition_variable.wait_until + 30s 兜底超时
    std::shared_ptr<Task> task;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        task = m_current_task;
    }
    if (task) {
        task->cancel();
        m_task_manager.get().wait(task);
    }
}

void ChatSession::set_system_prompt(const std::string& prompt) {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_system_prompt = prompt;
}

std::string ChatSession::system_prompt() const {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    return m_system_prompt;
}

void ChatSession::set_tool_registry(std::shared_ptr<tool::ToolRegistry> registry) {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_tool_registry = std::move(registry);
    // executor 由 ReActLoop 内部创建，此处仅保存 registry
}

std::shared_ptr<tool::ToolRegistry> ChatSession::tool_registry() const {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    return m_tool_registry;
}

void ChatSession::clear_history() {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_messages.clear();
    // DS_CACHE M-3：移除 m_cache_hit_total/m_cache_miss_total 重置（死代码已删除）
    // DS_CACHE M-5：重置压缩器状态（卡死守卫/rewrite_version），避免跨 clear_history 泄漏
    m_compactor.reset();
    // 重置前缀形状基线
    m_last_prefix_shape = PrefixShape{};
}

void ChatSession::set_compactor_context_window(int32_t context_window_tokens) {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_compactor.set_context_window(context_window_tokens);
}

void ChatSession::set_compactor_archive_dir(const std::string& dir) {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_compactor.set_archive_dir(dir);
}

// ============================================================
// 项目会话恢复：SessionStore 集成
// ============================================================

void ChatSession::set_session_store(std::shared_ptr<agent::session::SessionStore> store) {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_session_store = std::move(store);
}

void ChatSession::configure_session_store(const std::string& project_dir,
                                           const std::string& cwd,
                                           const std::string& model,
                                           const std::string& git_branch) {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_store_configured = true;
    m_store_project_dir = project_dir;
    m_store_cwd = cwd;
    m_store_model = model;
    m_store_git_branch = git_branch;
}

bool ChatSession::restore_from_file(const std::string& file_path) {
    auto messages = agent::session::SessionStore::load_messages(file_path);
    if (messages.empty()) return false;

    std::lock_guard<std::mutex> lock(m_state_mutex);
    // 追加到已有消息（不清空），支持恢复后继续对话
    m_messages.insert(m_messages.end(),
                      std::make_move_iterator(messages.begin()),
                      std::make_move_iterator(messages.end()));
    return true;
}

void ChatSession::import_messages(std::vector<ChatMessage> messages) {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_messages = std::move(messages);
    // 重置压缩器与前缀形状基线（对齐 switch_session，新上下文从零开始）
    m_compactor.reset();
    m_last_prefix_shape = PrefixShape{};
}

bool ChatSession::switch_session(const std::string& file_path) {    // 加载历史消息和元信息（文件 I/O 在锁外执行）
    auto messages = agent::session::SessionStore::load_messages(file_path);
    if (messages.empty()) return false;

    auto meta = agent::session::SessionStore::load_meta(file_path);
    if (!meta) return false;

    // 从文件名提取 session_id（stem，如 "76e1b10d-...-...jsonl" → "76e1b10d-...-..."）
    std::string new_session_id = std::filesystem::path(file_path).stem().string();

    std::lock_guard<std::mutex> lock(m_state_mutex);
    // 1. 替换 session_id
    m_session_id = new_session_id;

    // 2. 清空消息历史，填入加载的历史消息
    m_messages = std::move(messages);

    // 3. 关闭旧 SessionStore，创建新 SessionStore 指向历史文件
    if (m_session_store) {
        m_session_store->close();
    }
    auto new_store = std::make_shared<agent::session::SessionStore>(file_path, new_session_id);
    if (!new_store->open()) {
        return false;  // 打开失败，保留旧状态不变（messages 已替换但 store 为空，后续不持久化）
    }
    m_session_store = new_store;
    // 不追加 session_start（会话进行中，只是换文件继续写）

    // 4. 重置压缩器和前缀形状基线（新会话上下文从零开始）
    m_compactor.reset();
    m_last_prefix_shape = PrefixShape{};

    return true;
}

bool ChatSession::rename_session(const std::string& title) {
    std::shared_ptr<agent::session::SessionStore> store;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        store = m_session_store;
    }
    if (!store) return false;
    return store->append_title(title);
}

void ChatSession::persist_message(const ChatMessage& msg) {
    std::shared_ptr<agent::session::SessionStore> store;
    bool is_first_user = false;
    bool need_lazy_init = false;
    std::string lazy_project_dir, lazy_cwd, lazy_model, lazy_git_branch;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        store = m_session_store;

        // 检测是否是首条 user 消息（用于自动生成标题 + 懒创建触发条件）
        // 注意：当前 msg 在 persist_message 调用前已被 push_back 到 m_messages 中，
        //       因此用 user 消息计数 == 1 判断首条（而非检查"是否有 user 消息"）
        if (msg.role == ChatMessage::Role::User) {
            int user_count = 0;
            for (const auto& m : m_messages) {
                if (m.role == ChatMessage::Role::User) ++user_count;
            }
            is_first_user = (user_count == 1);
        }

        // 懒创建：首条 user 消息且 SessionStore 未创建时，创建文件 + 写 session_start
        if (!store && m_store_configured && is_first_user) {
            need_lazy_init = true;
            lazy_project_dir = m_store_project_dir;
            lazy_cwd = m_store_cwd;
            lazy_model = m_store_model;
            lazy_git_branch = m_store_git_branch;
        }

        if (!store && !need_lazy_init) return;
    }

    // 懒创建 SessionStore（锁外执行文件 I/O）
    if (need_lazy_init) {
        try {
            namespace fs = std::filesystem;
            fs::path session_file = fs::path(lazy_project_dir) / (m_session_id + ".jsonl");
            auto new_store = std::make_shared<agent::session::SessionStore>(
                session_file.string(), m_session_id);
            if (!new_store->open()) return;
            new_store->append_session_start(lazy_cwd, lazy_model, lazy_git_branch);
            // 写入 store 后再持久化消息
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                m_session_store = new_store;
            }
            store = new_store;
        } catch (const std::exception&) {
            return;  // 创建失败，放弃持久化
        }
    }

    if (!store) return;

    const std::string uuid = core::util::generate_uuid();
    const std::string timestamp = now_iso();

    switch (msg.role) {
        case ChatMessage::Role::User:
            store->append_user_message(uuid, "", msg.content, timestamp);
            // 首条 user 消息自动生成标题（前 20 字）
            if (is_first_user) {
                std::string title = msg.content;
                // UTF-8 安全截取前 20 字
                size_t char_count = 0;
                size_t byte_pos = 0;
                while (char_count < 20 && byte_pos < title.size()) {
                    unsigned char c = static_cast<unsigned char>(title[byte_pos]);
                    if (c < 0x80) byte_pos += 1;
                    else if ((c & 0xE0) == 0xC0) byte_pos += 2;
                    else if ((c & 0xF0) == 0xE0) byte_pos += 3;
                    else if ((c & 0xF8) == 0xF0) byte_pos += 4;
                    else byte_pos += 1;
                    ++char_count;
                }
                title = title.substr(0, byte_pos);
                if (byte_pos < msg.content.size()) {
                    title += "...";
                }
                store->append_title(title);
            }
            break;
        case ChatMessage::Role::Assistant:
            store->append_assistant_message(uuid, "", msg.content,
                                            msg.reasoning_content,
                                            msg.tool_uses, timestamp);
            break;
        case ChatMessage::Role::Tool:
            store->append_tool_message(uuid, "", msg.tool_call_id,
                                       msg.tool_name, msg.content,
                                       msg.is_error, timestamp);
            break;
        default:
            break;
    }
}

void ChatSession::persist_messages_range(size_t start_idx, const std::string& parent_uuid) {
    std::shared_ptr<agent::session::SessionStore> store;
    std::vector<ChatMessage> messages_copy;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        store = m_session_store;
        if (!store) return;
        if (start_idx >= m_messages.size()) return;
        messages_copy.assign(m_messages.begin() + start_idx, m_messages.end());
    }

    std::string current_parent = parent_uuid;
    for (const auto& msg : messages_copy) {
        const std::string uuid = core::util::generate_uuid();
        const std::string timestamp = now_iso();

        switch (msg.role) {
            case ChatMessage::Role::User:
                store->append_user_message(uuid, current_parent, msg.content, timestamp);
                break;
            case ChatMessage::Role::Assistant:
                store->append_assistant_message(uuid, current_parent, msg.content,
                                                msg.reasoning_content,
                                                msg.tool_uses, timestamp);
                break;
            case ChatMessage::Role::Tool:
                store->append_tool_message(uuid, current_parent, msg.tool_call_id,
                                           msg.tool_name, msg.content,
                                           msg.is_error, timestamp);
                break;
            default:
                break;
        }
        current_parent = uuid;  // 链式传递 parent_uuid
    }
}

std::string ChatSession::summarize_with_llm(const std::vector<ChatMessage>& middle) {
    // DS_CACHE M-4：同步调用 LLM 生成中段摘要
    // 由 compact_middle 在 compact 阶段调用；失败抛异常，由调用方 fallback 到机械折叠
    if (!m_provider) {
        throw std::runtime_error("summarize_with_llm: provider is null");
    }
    if (middle.empty()) {
        throw std::runtime_error("summarize_with_llm: empty middle");
    }

    // 构造摘要请求：system prompt 指示摘要任务 + middle 消息
    static const std::string k_summary_system_prompt =
        "你是对话历史压缩助手。请将以下历史对话（含用户问题、助手回答、工具调用结果）"
        "压缩为一份简洁的结构化摘要，保留：\n"
        "1. 用户的核心意图与关键约束\n"
        "2. 已完成的关键操作及其结果（工具名 + 要点）\n"
        "3. 未解决的问题或待办事项\n"
        "4. 关键文件路径、错误信息、数值参数\n"
        "用 <compaction-summary> 标签包裹摘要内容。不超过 500 字。不要添加新信息。";

    CompletionRequest req;
    // system prompt 作为首条消息（对齐主推理流程的 messages 约定）
    req.messages.reserve(middle.size() + 1);
    req.messages.push_back(ChatMessage::system(k_summary_system_prompt));
    for (const auto& msg : middle) {
        // 剥离图片附件：摘要请求只发文本（压缩模型可能不支持视觉）
        ChatMessage copy = msg;
        copy.image_paths.clear();
        req.messages.push_back(std::move(copy));
    }
    req.max_tokens = 1024;       // 摘要无需过长
    req.temperature = 0.3f;      // 低温度保证忠实
    req.stream = true;

    // 提交流式请求
    auto reader = m_provider->submit_completion(req);
    if (!reader) {
        throw std::runtime_error("summarize_with_llm: submit_completion returned null");
    }

    // 阻塞读取流直到完成，拼接 content
    std::string summary;
    StreamChunk chunk;
    auto should_stop = []() { return false; };  // 摘要不响应取消信号（短任务）
    while (true) {
        auto state = reader->next(should_stop, chunk);
        if (state == StreamState::HasData) {
            if (!chunk.content_delta.empty()) {
                summary += chunk.content_delta;
            }
            if (chunk.is_final) {
                break;
            }
        } else if (state == StreamState::Complete) {
            break;
        } else if (state == StreamState::Error) {
            throw std::runtime_error("summarize_with_llm: stream error");
        } else if (state == StreamState::Cancelled) {
            throw std::runtime_error("summarize_with_llm: stream cancelled");
        }
    }

    if (summary.empty()) {
        throw std::runtime_error("summarize_with_llm: empty summary");
    }

    // 确保 <compaction-summary> 包裹（LLM 可能不严格遵循 system prompt）
    if (summary.find("<compaction-summary>") == std::string::npos) {
        summary = "<compaction-summary>\n" + summary + "\n</compaction-summary>";
    }

    return summary;
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
    std::vector<std::string> last_user_images;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        while (!m_messages.empty() &&
               m_messages.back().role == ChatMessage::Role::Assistant) {
            m_messages.pop_back();
        }
        if (!m_messages.empty() &&
            m_messages.back().role == ChatMessage::Role::User) {
            last_user_text = m_messages.back().content;
            last_user_images = m_messages.back().image_paths;
            m_messages.pop_back();
        } else {
            return;  // 没有 user 消息可重生成
        }
    }
    run_completion(last_user_text, last_user_images);
}

void ChatSession::send_message(const std::string& text,
                               const std::vector<std::string>& images) {
    if (m_generating.load()) {
        m_event_bus.get().publish_async(StreamErrorEvent{
            .session_id = m_session_id,
            .message = "Still generating, please wait or press Ctrl+C to interrupt",
            .retryable = true
        });
        return;
    }

    run_completion(text, images);
}

std::vector<ChatMessage> ChatSession::get_messages() const {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    return m_messages;
}

// DS_CACHE M-3：session_cache_stats() 已删除（死代码，TUI 使用 token_stats_model）

void ChatSession::run_completion(const std::string& user_text,
                                 const std::vector<std::string>& images,
                                 int retry_attempt) {
    // B.1：拆分后的 run_completion 仅做顶层调度，agent 循环逻辑分发到子方法
    // 子方法返回 AgentStepResult，决定下一步动作（避免 goto 跨变量声明）

    // 仅首次请求时添加用户消息（重试时不重复添加）
    if (retry_attempt == 0) {
        ChatMessage user_msg = images.empty()
            ? ChatMessage::user(user_text)
            : ChatMessage::user(user_text, images);
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            m_messages.push_back(user_msg);
        }
        // 项目会话恢复：用户消息实时持久化
        persist_message(user_msg);
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
            // BashTool DI：注入 TaskManager，工具通过 ToolContext.task_manager() 启动后台任务
            // cwd：注入会话启动时捕获的工作目录，避免运行中 cwd 漂移导致工具在错误目录执行
            // DS_CACHE H-3：注入 m_compactor 引用，使卡死守卫/rewrite_version 跨 turn 持久化
            // AskUserTool DI：注入 EventBus，工具通过 ToolContext.event_bus() 发布事件
            ReActLoop loop(m_provider.get(), m_tool_registry, ReActLoop::Config{},
                           &m_config_manager.get(), &m_task_manager.get(), m_cwd,
                           &m_compactor, &m_event_bus.get());

            // 3.2：使用 IReActObserver 接口替代 lambda 回调
            // ReActEventPublisher 内部完成 ReActStep → IEventBus 事件转换
            ReActEventPublisher publisher(m_event_bus, m_session_id);

            // ---- DS_CACHE: 捕获前缀形状（用于本轮结束后的缓存劣化归因）----
            // H-2：cur_shape 在 run() 后二次捕获，以传入压缩器 rewrite_version。
            //      prev_shape 从上一轮 m_last_prefix_shape 读取（含上轮的 rewrite_version）。
            PrefixShape prev_shape;
            size_t messages_before_loop = 0;  // 项目会话恢复：记录 loop 前消息数，用于批量持久化
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                prev_shape = m_last_prefix_shape;
                messages_before_loop = m_messages.size();
            }
            // 预捕获仅用于 prev_shape 为空（首轮）时的 prefix_hash 基线；
            // 真正的 cur_shape 在 run() 返回后用 react_result.rewrite_version 二次捕获
            PrefixShape cur_shape_baseline = capture_shape(m_system_prompt, tools_schema, 0);
            if (prev_shape.prefix_hash.empty()) {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                m_last_prefix_shape = cur_shape_baseline;
            }

            // ---- 执行 ReAct 循环 ----
            ReActResult react_result = loop.run(
                m_messages, m_system_prompt, tools_schema,
                should_cancel, &publisher
            );

            // 项目会话恢复：批量持久化 ReActLoop 新增的 assistant/tool 消息
            persist_messages_range(messages_before_loop);

            // H-2：用 react_result.rewrite_version 二次捕获 cur_shape，使 log_rewrite 归因生效
            PrefixShape cur_shape = capture_shape(m_system_prompt, tools_schema,
                                                  react_result.rewrite_version);
            {
                std::lock_guard<std::mutex> lock(m_state_mutex);
                m_last_prefix_shape = cur_shape;
            }

            // ============================================================
            // 结果处理
            // ============================================================

            // ---- 用户中断 ----
            if (react_result.was_interrupted) {
                if (!react_result.partial_content.empty()) {
                    ChatMessage partial_msg = ChatMessage::assistant(react_result.partial_content);
                    if (!react_result.partial_reasoning.empty()) {
                        partial_msg.reasoning_content = react_result.partial_reasoning;
                    }
                    {
                        std::lock_guard<std::mutex> lock(m_state_mutex);
                        m_messages.push_back(partial_msg);
                    }
                    // 项目会话恢复：持久化中断时的 partial 消息
                    persist_message(partial_msg);
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
                    .prompt_cache_hit_tokens = react_result.prompt_cache_hit_tokens,
                    .prompt_cache_miss_tokens = react_result.prompt_cache_miss_tokens,
                    .prompt_ms = react_result.prompt_ms,
                    .generation_ms = react_result.generation_ms
                });
                m_generating.store(false);
                return;
            }

            // ---- 错误处理 ----
            // H-7：纯函数 compute_retry 决策，I/O 由 run_completion 执行
            if (react_result.was_error) {
                auto decision = compute_retry(react_result, retry_policy, retry_attempt);

                switch (decision.action) {
                case RetryAction::Sleep: {
                    // 可重试：发布重试提示事件 + 可中断等待 + 递归重试
                    m_event_bus.get().publish_async(StreamErrorEvent{
                        .session_id = m_session_id,
                        .message = std::format(
                            "Error: {}, retrying in {}ms... ({}/{})",
                            react_result.error_message, decision.delay_ms,
                            retry_attempt + 1, retry_policy.max_retries),
                        .retryable = true
                    });

                    // 可中断的等待
                    auto wait_until = std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds(decision.delay_ms);
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
                        run_completion(user_text, {}, retry_attempt + 1);
                    }
                    break;
                }
                case RetryAction::Stop:
                    // 不可重试：发布终止事件
                    m_event_bus.get().publish_async(StreamErrorEvent{
                        .session_id = m_session_id,
                        .message = react_result.error_message,
                        .retryable = false
                    });
                    break;
                case RetryAction::Continue:
                    // 无错误路径，理论上不应进入（was_error=true 时不会返回 Continue）
                    break;
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
                .prompt_cache_hit_tokens = react_result.prompt_cache_hit_tokens,
                .prompt_cache_miss_tokens = react_result.prompt_cache_miss_tokens,
                .prompt_ms = react_result.prompt_ms,
                .generation_ms = react_result.generation_ms
            });

            // DS_CACHE M-3：移除 m_cache_hit_total/m_cache_miss_total 累加（死代码已删除）
            // TUI 通过 token_stats_model 的 update_from_usage 自行累计

            // DS_CACHE: 发布缓存诊断事件（前缀变化归因）
            // 仅当前缀变化或本轮有 cache 数据时发布，避免无意义事件刷屏
            if (!prev_shape.prefix_hash.empty() || react_result.prompt_cache_hit_tokens > 0
                || react_result.prompt_cache_miss_tokens > 0) {
                auto diag = compare_shape(prev_shape, cur_shape,
                                          react_result.prompt_cache_hit_tokens,
                                          react_result.prompt_cache_miss_tokens);
                if (diag.prefix_changed || diag.cache_miss_tokens > 0) {
                    m_event_bus.get().publish_async(CacheDiagnosticsEvent{
                        .session_id = m_session_id,
                        .prefix_hash = diag.prefix_hash,
                        .prefix_changed = diag.prefix_changed,
                        .reasons = diag.reasons,
                        .cache_hit_tokens = diag.cache_hit_tokens,
                        .cache_miss_tokens = diag.cache_miss_tokens
                    });
                }
            }

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

// ============================================================
// H-7：compute_retry 纯函数（重试决策）
// ============================================================

RetryDecision ChatSession::compute_retry(const ReActResult& react_result,
                                         const HttpRetryPolicy& retry_policy,
                                         int attempt) {
    // 无错误：继续执行（非错误路径）
    if (!react_result.was_error) {
        return RetryDecision{RetryAction::Continue, 0};
    }

    // 可重试判定：①未超 max_retries ②HttpRetryPolicy.is_retryable 通过
    // http_status=0 表示业务错误（非 HTTP），由 error_message 内容判断
    const bool can_retry = attempt < retry_policy.max_retries
                           && HttpRetryPolicy::is_retryable(0, react_result.error_message);

    if (!can_retry) {
        return RetryDecision{RetryAction::Stop, 0};
    }

    // 退避延迟计算（委托 HttpRetryPolicy.delay_ms，含 60s 上限）
    return RetryDecision{RetryAction::Sleep, retry_policy.delay_ms(attempt)};
}

// ============================================================
// H-6：序列化/反序列化纯函数
// ============================================================

nlohmann::json ChatSession::serialize_state() const {
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

    // H-6 修复：显式初始化 messages 为数组，避免空消息时 j["messages"] 为 null
    // （原代码 auto& messages = j["messages"]; 在空消息时遗留 null 值）
    nlohmann::json messages = nlohmann::json::array();
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
            if (msg.is_error) {
                m["is_error"] = true;
            }
        }
        // Task 9：序列化 assistant 消息的 tool_uses（保证 /save /load 后不丢失）
        if (msg.role == ChatMessage::Role::Assistant && !msg.tool_uses.empty()) {
            nlohmann::json uses = nlohmann::json::array();
            for (const auto& tu : msg.tool_uses) {
                uses.push_back({{"id", tu.id}, {"name", tu.name}, {"input", tu.input}});
            }
            m["tool_uses"] = std::move(uses);
        }
        messages.push_back(m);
    }
    j["messages"] = std::move(messages);

    return j;
}

// C-1：改为真正纯函数——不修改任何成员状态，仅解析 j 返回 (messages, system_prompt)
Result<std::pair<std::vector<ChatMessage>, std::string>, std::string>
ChatSession::deserialize_state(const nlohmann::json& j) {
    std::vector<ChatMessage> new_messages;
    std::string new_system_prompt;

    try {
        if (j.contains("system_prompt")) {
            new_system_prompt = j["system_prompt"].get<std::string>();
        }

        if (j.contains("messages")) {
            for (const auto& m : j["messages"]) {
                ChatMessage msg;

                // H-6：const operator[] 在 missing key 上触发 assert，必须先 contains 检查
                if (!m.contains("role")) {
                    return Result<std::pair<std::vector<ChatMessage>, std::string>, std::string>::err(
                        "Message missing 'role' field");
                }
                if (!m.contains("content")) {
                    return Result<std::pair<std::vector<ChatMessage>, std::string>, std::string>::err(
                        "Message missing 'content' field");
                }

                std::string role_str = m["role"].get<std::string>();
                if (role_str == "system")          msg.role = ChatMessage::Role::System;
                else if (role_str == "user")       msg.role = ChatMessage::Role::User;
                else if (role_str == "assistant")  msg.role = ChatMessage::Role::Assistant;
                else if (role_str == "tool")       msg.role = ChatMessage::Role::Tool;
                else {
                    return Result<std::pair<std::vector<ChatMessage>, std::string>, std::string>::err(
                        std::format("Unknown role: {}", role_str));
                }

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
                    if (m.contains("is_error")) {
                        msg.is_error = m["is_error"].get<bool>();
                    }
                }
                // Task 9：反序列化 assistant 消息的 tool_uses
                if (msg.role == ChatMessage::Role::Assistant && m.contains("tool_uses")) {
                    for (const auto& tu : m["tool_uses"]) {
                        ToolUse use;
                        use.id = tu.value("id", "");
                        use.name = tu.value("name", "");
                        use.input = tu.value("input", nlohmann::json::object());
                        msg.tool_uses.push_back(std::move(use));
                    }
                }
                new_messages.push_back(std::move(msg));
            }
        }
    } catch (const nlohmann::json::parse_error& e) {
        return Result<std::pair<std::vector<ChatMessage>, std::string>, std::string>::err(
            std::format("JSON parse error: {}", e.what()));
    } catch (const nlohmann::json::type_error& e) {
        return Result<std::pair<std::vector<ChatMessage>, std::string>, std::string>::err(
            std::format("JSON type error: {}", e.what()));
    } catch (const std::exception& e) {
        return Result<std::pair<std::vector<ChatMessage>, std::string>, std::string>::err(
            std::format("Error deserializing session: {}", e.what()));
    }

    return Result<std::pair<std::vector<ChatMessage>, std::string>, std::string>::ok(
        std::make_pair(std::move(new_messages), std::move(new_system_prompt)));
}

// C-1：一次性加锁提交状态到成员
void ChatSession::commit_state(std::vector<ChatMessage> messages,
                                std::string system_prompt) {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_messages = std::move(messages);
    m_system_prompt = std::move(system_prompt);
}

// ============================================================
// H-6：save/load 仅做 I/O，序列化逻辑委托纯函数
// ============================================================

Result<void, std::string> ChatSession::save_session(const std::string& path) const {
    try {
        nlohmann::json j = serialize_state();

        std::filesystem::path p(path);
        if (p.has_parent_path()) {
            std::filesystem::create_directories(p.parent_path());
        }

        std::ofstream file(path);
        if (!file.is_open()) {
            return Result<void, std::string>::err(
                std::format("Failed to create file: {}", path));
        }
        file << j.dump(2);
        file.close();
        return Result<void, std::string>::ok();

    } catch (const std::exception& e) {
        return Result<void, std::string>::err(
            std::format("Error saving session: {}", e.what()));
    }
}

Result<void, std::string> ChatSession::load_session(const std::string& path) {
    if (!std::filesystem::exists(path)) {
        return Result<void, std::string>::err(
            std::format("File not found: {}", path));
    }

    try {
        std::ifstream file(path);
        if (!file.is_open()) {
            return Result<void, std::string>::err(
                std::format("Failed to open file: {}", path));
        }

        nlohmann::json j;
        file >> j;
        file.close();

        // C-1：deserialize_state 是纯函数，需通过 commit_state 提交到成员
        auto parse_result = deserialize_state(j);
        if (parse_result.isErr()) {
            return Result<void, std::string>::err(parse_result.error());
        }
        auto [messages, system_prompt] = std::move(parse_result).unwrap();
        commit_state(std::move(messages), std::move(system_prompt));
        return Result<void, std::string>::ok();

    } catch (const nlohmann::json::parse_error& e) {
        return Result<void, std::string>::err(
            std::format("JSON parse error: {}", e.what()));
    } catch (const std::exception& e) {
        return Result<void, std::string>::err(
            std::format("Error loading session: {}", e.what()));
    }
}

// H-8：backend() 方法已删除，UI 层通过 factory 注入的 IBackendAdmin* 调用管理接口。
// 详见 factory.cpp 中 SessionResult.backend_admin 字段。

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

