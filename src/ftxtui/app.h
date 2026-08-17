/**
 * @file app.h
 * @brief App — 顶层组件树与事件驱动装配
 * @details 持有 ViewModel / ActionQueue / EventBridge，构建 ftxui 组件树，
 *          每帧 drain action 并重绘。输入经回调交回 main 的会话处理。
 */

#pragma once

#include <atomic>
#include <chrono>
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
#include "bridge/action_queue.h"
#include "bridge/event_bridge.h"
#include "render/markdown_to_elements.h"  // CardHit
#include "vm/view_model.h"

namespace agent {
class ChatSession;
class IBackendAdmin;
class IEventBus;
}  // namespace agent

namespace agent::command {
class CommandRegistry;
}  // namespace agent::command

namespace ftxtui {

/// @brief App 依赖（从 main 注入）
struct AppDeps {
    agent::ChatSession* session = nullptr;
    agent::IBackendAdmin* backend_admin = nullptr;
    agent::IEventBus* event_bus = nullptr;
    bool mock_mode = false;  ///< --mock：无后端演示（send_input 走本地 mock 流）
    std::string model_name;
    std::string project;
    std::string agent_name;
    std::string session_dir;  ///< 项目会话目录（/resume 列出历史用）
    /// @brief 命令注册表（命令面板数据源；斜杠命令经 InputProcessor 执行）
    std::shared_ptr<agent::command::CommandRegistry> command_registry;

    /// @brief 提交一条用户消息（由 main 的实现路由到会话/处理器）
    std::function<void(const std::string&)> on_submit;

    /// @brief 请求模型切换后回调（main 注入，刷新侧栏/状态行）
    std::function<void()> on_model_changed;

    /// @brief 退出应用回调（/exit）
    std::function<void()> on_exit;
};

/// @brief 顶层应用
class App {
public:
    explicit App(AppDeps deps);
    ~App();

    /// @brief 运行 ftxui 主循环（阻塞）
    void run();

private:
    void drain();
    void handle_ask_user(const ActionAskUser& a);
    void close_ask(bool submitted);
    void send_input(const std::string& text);
    void start_mock_stream(const std::string& user_text);
    void cmd_resume(const std::string& args);
    void cmd_rename(const std::string& args);
    void open_model_selector();
    void apply_model(int index);
    int approx_height(int width) const;
    static int layout_rows(const MessageNode& m, int y_base);
    ftxui::Element build_transcript(int width);
    ftxui::Element build_ask_modal() const;
    static std::string mode_label(agent::tool::PermissionMode m);

    AppDeps m_deps;
    ViewModel m_vm;
    ActionQueue m_queue;
    EventBridge m_bridge;
    ftxui::ScreenInteractive m_screen;

    /// @brief 折叠卡片渲染后的屏幕位置（每帧由 build_transcript 重建）
    // 用 deque：reflect 持有 Box&（CardHit 元素引用），deque push_back 不使已有元素引用失效
    std::deque<CardHit> m_hits;

    /// @brief 追加一行运行时日志（codex_run.log，多线程安全）
    void log_run(std::string_view msg);
    std::mutex m_log_mutex;

    // 输入缓冲与面板状态
    std::string m_input_buffer;
    std::string m_ask_buffer;          ///< AskUser 模态输入
    bool m_palette_open = false;
    bool m_model_open = false;
    int m_scroll = 0;
    bool m_follow = true;              ///< 自动跟随最新内容
    bool m_ask_active = false;
    std::string m_ask_question;
    std::shared_ptr<std::promise<agent::AskUserResult>> m_ask_promise;
    std::chrono::steady_clock::time_point m_ask_deadline;

    std::vector<std::string> m_model_items;  ///< 模型列表（/model 面板）
    int m_mock_perm_cycle = 0;   ///< mock 下 Shift+Tab 权限循环序号（""→plan→bypass）

    // 思考动画（busy 时推进帧并持续重绘）
    std::size_t m_anim_frame = 0;        ///< 动画帧号（UI 线程自增）
    std::atomic<bool> m_anim_run{false}; ///< 动画线程运行标志
    std::atomic<bool> m_busy{false};     ///< 忙标志（UI 线程写，动画线程读）
    std::thread m_anim_thread;           ///< 动画推进线程（busy 时发 Event::Custom）

    // mock 流式输出（后台线程逐步入队 token，模拟 LLM 流式回复）
    std::atomic<bool> m_stream_run{false}; ///< 流式线程运行标志
    std::thread m_stream_thread;           ///< 流式线程

    // 组件
    ftxui::Component m_transcript;
    ftxui::Component m_sidebar;
    ftxui::Component m_status;
    ftxui::Component m_composer;
    ftxui::Component m_ask_input;
    ftxui::Component m_palette_comp;
    ftxui::Component m_model_comp;
};

}  // namespace ftxtui