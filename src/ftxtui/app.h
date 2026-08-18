/**
 * @file app.h
 * @brief App — 顶层组件树与事件驱动装配
 * @details 持有 ViewModel / ActionQueue / EventBridge，构建 ftxui 组件树，
 *          每帧 drain action 并重绘。输入经回调交回 main 的会话处理。
 */

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/component/component.hpp>

#include "core/events/agent_events.h"
#include "agent/tool/context.h"  // tool::PermissionMode
#include "agent/factory.h"       // agent::BackendCreateResult
#include "agent/model/provider_config.h"  // agent::ProviderConfigEntry
#include "bridge/action_queue.h"
#include "bridge/event_bridge.h"
#include "render/markdown_to_elements.h"  // CardHit
#include "vm/view_model.h"
#include "widgets/suggest_panel.h"
#include "widgets/search_palette.h"
#include "core/utils/file_index.h"  // agent::FileIndex::Entry

namespace agent {
class ChatSession;
class IBackendAdmin;
class ICompletionProvider;
class IEventBus;
class IConfigManager;
}  // namespace agent

namespace agent::command {
class CommandRegistry;
}  // namespace agent::command

namespace agent::input {
class InputProcessor;
}  // namespace agent::input

namespace ftxtui {

/// @brief App 依赖（从 main 注入）
struct AppDeps {
    agent::ChatSession* session = nullptr;
    agent::IBackendAdmin* backend_admin = nullptr;
    agent::IEventBus* event_bus = nullptr;
    agent::IConfigManager* config_manager = nullptr;  ///< 配置（供应商列表 / 当前 provider）
    bool mock_mode = false;  ///< --mock：无后端演示（send_input 走本地 mock 流）
    bool smoke_mode = false; ///< --smoke：自动驱动一轮 mock 对话后退出（CI 冒烟）
    std::string model_name;
    std::string project;
    std::string agent_name;
    std::string session_dir;  ///< 项目会话目录（/resume 列出历史用）
    /// @brief 命令注册表（内置 + 用户命令；斜杠命令经统一命令执行路径）
    std::shared_ptr<agent::command::CommandRegistry> command_registry;

    /// @brief 提交一条用户消息（由 main 的实现路由到会话/处理器）
    std::function<void(const std::string&)> on_submit;

    /// @brief 请求模型切换后回调（main 注入，刷新侧栏/状态行）
    std::function<void()> on_model_changed;

    /// @brief 退出应用回调（/exit）
    std::function<void()> on_exit;

    /// @brief 运行时创建后端（/provider 热切换；main 注入 agent::create_backend 包装）
    /// @param provider_name 供应商 id（ProviderConfigEntry.id，写入 backend.provider）
    /// @return BackendCreateResult（provider 可为空 = 配置不足或创建/初始化失败）
    std::function<agent::BackendCreateResult(const std::string& provider_name)> create_provider;
};

/// @brief 顶层应用
class App {
public:
    explicit App(AppDeps deps);
    ~App();

    /// @brief 运行 ftxui 主循环（阻塞）
    void run();

    /// @brief 退出码（冒烟模式：0=通过，1=超时/失败；常规模式恒 0）
    int exit_code() const { return m_exit_code.load(); }

private:
    void drain();
    void handle_ask_user(const ActionAskUser& a);
    void close_ask(bool submitted);
    /// @brief 记录当前题答案并前进；全部答完则提交（Enter 确认路径）
    bool advance_ask();
    /// @brief 冒烟驱动（B5）：自动投递 mock 对话，完成后请求退出
    void start_smoke_driver();
    void send_input(const std::string& text);
    /// @brief 统一命令执行入口（斜杠命令经 InputProcessor → CommandExecutor）
    void run_command(const std::string& cmd, const std::string& args);
    void start_mock_stream(const std::string& user_text);
    void cmd_resume(const std::string& args);
    void cmd_rename(const std::string& args);
    /// @brief 恢复指定会话（switch_session + 历史载入；cmd_resume 与搜索面板共用）
    void resume_session(const std::string& file_path, const std::string& title);
    void open_model_selector();
    void apply_model(int index);
    /// @brief 从 m_model_items 重建 /model 面板条目（active = 当前模型）
    void rebuild_model_entries();
    /// @brief 打开 /resume 会话选择面板（仅会话条目）
    void open_resume_palette();
    /// @brief 打开供应商切换面板（读配置 backend.providers）
    void open_provider_palette();
    /// @brief 供应商面板选中：后台创建后端 → 完成后热切换
    void switch_provider(int index);
    /// @brief 供应商切换完成（UI 线程）：set_provider + 保留对话 + 写配置
    void handle_provider_switched(std::unique_ptr<agent::ICompletionProvider> provider,
                                  const std::string& model_name,
                                  const agent::ProviderConfigEntry& entry);
    /// @brief 供应商切换失败提示
    void handle_provider_switch_failed(const std::string& provider_name);
    /// @brief 会话列表懒加载（后台线程 list_sessions → ActionSessionsLoaded）
    void ensure_sessions_loaded();
    /// @brief 从 m_providers 装配供应商面板条目（active = 当前 provider）
    void assemble_provider_entries();
    /// @brief 从输入缓冲推导提示面板状态并过滤（每帧 / 字符变化后调用）
    void update_suggest();
    /// @brief 提示面板移动选择（delta=±1；Tab 传 +1，到底循环回首）
    void suggest_move(int delta);
    /// @brief 提示面板 Enter 确认（命令模式=运行；文件模式=插入路径）
    /// @return true=已消费（不提交发送）
    bool suggest_enter();
    /// @brief 关闭提示面板（Esc）
    void suggest_cancel();
    /// @brief 装配聚合搜索条目（功能/文件/会话/设置），会话未加载时触发后台加载
    std::vector<SearchEntry> assemble_search_entries();
    /// @brief 执行聚合搜索面板选中项（按类别分发）
    void apply_search_entry(int index);
    /// @brief 执行设置动作（搜索面板「设置」类）
    void run_setting(int action);
    int approx_height(int width) const;
    ftxui::Element build_transcript(int width);
    ftxui::Element build_ask_modal() const;
    static std::string mode_label(agent::tool::PermissionMode m);

    AppDeps m_deps;
    ViewModel m_vm;
    ActionQueue m_queue;
    EventBridge m_bridge;
    ftxui::ScreenInteractive m_screen;
    /// @brief 命令处理器（B2 统一：所有输入经它分派；持有 agent 注册表）
    std::unique_ptr<agent::input::InputProcessor> m_command_processor;

    /// @brief 折叠卡片渲染后的屏幕位置（每帧由 build_transcript 重建）
    // 用 deque：reflect 持有 Box&（CardHit 元素引用），deque push_back 不使已有元素引用失效
    std::deque<CardHit> m_hits;

    /// @brief 追加一行运行时日志（codex_run.log，多线程安全）
    void log_run(std::string_view msg);
    std::mutex m_log_mutex;

    // 输入缓冲与面板状态
    std::string m_input_buffer;
    std::string m_ask_buffer;          ///< AskUser 模态输入（自定义输入模式用）
    bool m_palette_open = false;
    bool m_model_open = false;
    int m_scroll = 0;
    bool m_follow = true;              ///< 自动跟随最新内容

    // ---- 输入栏提示面板（/ 命令 · @ 文件）----
    size_t m_composer_cursor = 0;      ///< composer 光标（外部持有，供路径替换同步）
    SuggestMode m_suggest_mode = SuggestMode::None;
    std::vector<SuggestEntry> m_suggest_entries;   ///< 过滤后的候选
    std::vector<agent::FileIndex::Entry> m_suggest_files;  ///< 文件候选（payload 映射）
    int m_suggest_selected = -1;       ///< 选中项下标（-1 = 无）

    // ---- 聚合搜索面板（Ctrl+P）----
    std::vector<PaletteCommand> m_palette_cmds;      ///< 命令条目（注册表派生，搜索/提示共用）
    std::vector<SearchEntry> m_search_entries;   ///< 面板打开时装配的条目（on_select 映射）
    std::vector<SessionLite> m_session_metas;    ///< 会话列表缓存（后台加载）
    bool m_sessions_loading = false;             ///< 会话列表正在后台加载

    // ---- 统一悬浮面板：/resume 会话 · /model 模型 · /provider 供应商 ----
    std::vector<SearchEntry> m_session_entries;  ///< /resume 面板条目（仅会话）
    bool m_resume_open = false;
    std::vector<SearchEntry> m_model_entries;    ///< /model 面板条目（由 m_model_items 派生）
    std::vector<SearchEntry> m_provider_entries; ///< 供应商面板条目
    bool m_provider_open = false;
    std::vector<agent::ProviderConfigEntry> m_providers;  ///< 配置中的供应商列表
    std::string m_current_provider;              ///< 当前供应商 id（backend.provider）

    // ---- AskUser 模态（B3：多问题 + 选项 + 自定义输入 + cancel_flag）----
    struct AskQuestion {
        std::string question;          ///< 完整问题文本（答案 map 的 key）
        std::string header;            ///< 短标签（可空，回退用 question）
        bool multi = false;            ///< 多选（Space 勾选；Enter 确认当前题）
        bool allow_custom_input = true;
        std::vector<std::string> options;  ///< 选项 label
    };
    bool m_ask_active = false;
    size_t m_ask_qindex = 0;           ///< 当前问题下标
    int m_ask_sel = 0;                 ///< 当前选中选项下标（-1 = 无）
    std::vector<bool> m_ask_checked;   ///< 多选勾选状态（仅 multi 用）
    bool m_ask_custom = false;         ///< 是否处于自定义输入模式
    std::vector<AskQuestion> m_ask_questions;  ///< 解析后的问题列表
    std::vector<std::pair<std::string, std::string>> m_ask_answers;  ///< 已答（question→answer）
    std::shared_ptr<std::promise<agent::AskUserResult>> m_ask_promise;
    std::shared_ptr<std::atomic<bool>> m_ask_cancel;  ///< 取消标志（工作线程超时置位）
    std::chrono::steady_clock::time_point m_ask_deadline;

    std::vector<std::string> m_model_items;  ///< 模型列表（/model 面板）
    int m_mock_perm_cycle = 0;   ///< mock 下 Shift+Tab 权限循环序号（""→plan→bypass）

    // 思考动画（busy 时推进帧并持续重绘）
    std::size_t m_anim_frame = 0;        ///< 动画帧号（UI 线程自增）
    std::atomic<bool> m_anim_run{false}; ///< 动画线程运行标志
    std::atomic<bool> m_busy{false};     ///< 忙标志（UI 线程写，动画线程读）
    std::mutex m_anim_mutex;             ///< 动画线程等待锁（IDLE 睡眠）
    std::condition_variable m_anim_cv;   ///< busy 变化唤醒动画线程
    std::thread m_anim_thread;           ///< 动画推进线程（busy 时发 Event::Custom）

    // mock 流式输出（后台线程逐步入队 token，模拟 LLM 流式回复）
    std::atomic<bool> m_stream_run{false}; ///< 流式线程运行标志
    std::thread m_stream_thread;           ///< 流式线程

    // 冒烟模式（B5）状态：driver 线程经 atomic 与 UI 线程通信
    std::atomic<int> m_exit_code{0};       ///< 退出码（0=通过；1=超时）
    std::atomic<bool> m_smoke_submit{false}; ///< 置位=UI 线程应投递冒烟消息
    std::atomic<bool> m_smoke_exit{false};   ///< 置位=UI 线程应请求退出
    std::atomic<size_t> m_msg_count{0};      ///< 消息数镜像（drain 更新，driver 只读）

    // 组件
    ftxui::Component m_transcript;
    ftxui::Component m_sidebar;
    ftxui::Component m_status;
    ftxui::Component m_composer;
    ftxui::Component m_ask_input;
    ftxui::Component m_palette_comp;
    ftxui::Component m_resume_comp;
    ftxui::Component m_model_comp;
    ftxui::Component m_provider_comp;
};

}  // namespace ftxtui