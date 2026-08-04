/**
 * @file terminal.h
 * @brief Terminal 类
 * @details 封装 IPlatform + LineEditor + 颜色输出 + 输入回调 + EventBus 集成
 * @version 2.0.0
 */

#pragma once

#include <string>
#include <string_view>
#include <memory>
#include <functional>
#include <mutex>
#include <thread>
#include <atomic>
#include <optional>
#include <future>

#include <nlohmann/json.hpp>

#include "core/utils/result.h"
#include "tui/core/color_scheme.h"
#include "tui/input/history.h"
#include "tui/core/display_buffer.h"
#include "core/events/i_event_bus.h"
#include "core/config/i_config_manager.h"
#include "core/task/task_manager.h"  // ITaskManager + TaskManager::instance() 默认实参

namespace tui {

class IPlatform;
class Screen;
class LineEditor;
class Spinner;
class BottomBarManager;
class DisplayBuffer;
class StatusBar;
class ChatRenderer;
struct ChoiceResult;

namespace detail {
/// @brief 待处理的 AskUser 请求（由事件泵线程设置，主循环消费）
struct PendingAskRequest {
    nlohmann::json questions;
    std::shared_ptr<std::promise<ChoiceResult>> result_promise;
    /// @brief 取消标志：工作线程超时后置位，run_choice_panel 检查后关闭面板
    std::shared_ptr<std::atomic<bool>> cancel_flag;
};
}  // namespace detail

/**
 * @brief Terminal 配置
 */
struct TerminalConfig {
    bool simple_io = false;        ///< 简单 I/O 模式（getline）
    bool use_color = true;         ///< 启用颜色
    std::string prompt_string = "\xe2\x9d\xaf ";  ///< ❯ (U+276F)
    bool multiline_input = false;  ///< 多行输入
};

/**
 * @brief 补全回调类型
 */
using CompletionCallback = std::function<std::vector<std::pair<std::string, size_t>>(
    std::string_view line, size_t cursor_pos)>;

/**
 * @brief 终端类
 *
 * 封装平台 I/O、行编辑、颜色控制和事件发布。
 * 主循环在 run() 中阻塞读取输入，通过 EventBus 发布事件。
 */
class Terminal {
public:
    using InputCallback = std::function<void(const std::string&)>;

    /// @brief 构造 Terminal
    /// @param event_bus 事件总线（H-4：必须显式注入，不再回退单例；
    ///                     传入指针用于支持可空场景，但调用 event_bus() 时必须非空）
    /// @param config_manager 配置管理器（H-4：必须显式注入，不再回退单例）
    /// @param task_manager 任务管理器（H-4：必须显式注入，不再回退单例）
    /// @param config 终端配置
    explicit Terminal(agent::IEventBus* event_bus,
                      agent::IConfigManager* config_manager,
                      agent::ITaskManager* task_manager,
                      const TerminalConfig& config = TerminalConfig{});
    ~Terminal();

    /// @brief 初始化终端（raw mode、平台设置、滚动区域）
    agent::Result<void, std::string> initialize();

    /// @brief 设置 ANSI 滚动区域，保护底部输入行和状态栏
    void setup_scroll_region();

    /// @brief setup_scroll_region 的不持锁版本（调用方必须已持有 m_output_mutex）
    /// @details 供 handle_resize() 在单一锁内调用，避免分段加锁的窗口期
    void setup_scroll_region_locked();

    /// @brief 仅更新滚动区下界（编辑期间输入区行数变化），不移动光标
    void update_scroll_region_bottom();

    /// @brief 滚动区下界行号（考虑编辑期间输入区行数）
    int scroll_region_bottom() const;

    /// @brief 是否处于多行输入编辑（输入区覆盖状态栏行，状态栏应暂停渲染）
    bool is_multiline_editing() const { return m_editing && m_edit_lines > 1; }

    /// @brief 重置滚动区域为全屏
    void reset_scroll_region();

    /// @brief 将光标移入输出区（滚动区域内最后一行）
    void cursor_to_output();

    /// @brief 标记光标已离开输出区（如定位到输入行/状态栏行）
    /// @details 后续 write() 会先将光标归位到输出区再写入
    void mark_cursor_left_output();

    /// @brief 恢复终端原始设置
    void restore();

    /// @brief 主输入循环（阻塞）
    void run();

    /// @brief 关闭终端
    void shutdown();

    // ---- AskUser 跨线程协作 ----

    /// @brief 设置待处理的 AskUser 请求（事件泵线程调用）
    /// @details ChatRenderer 订阅 AskUserRequestEvent 回调中调用，
    ///          存入 pending 请求并唤醒主循环。主循环取出后弹出 ChoicePanel。
    void set_pending_ask(detail::PendingAskRequest req);

    /// @brief 取出待处理的 AskUser 请求（主循环调用）
    /// @return 待处理请求；无请求返回空 optional
    std::optional<detail::PendingAskRequest> take_pending_ask();

    /// @brief 唤醒阻塞在 read_char 的主循环（线程安全）
    void wake_main_loop();

    // ---- 渲染 API ----

    /// @brief 设置颜色
    void set_color(ColorRole role);

    /// @brief 重置颜色
    void reset_color();

    /// @brief 输出文本
    void write(std::string_view text);

    /// @brief 线程安全的移动光标
    void move_cursor_safe(int cols);

    /// @brief 线程安全的输出文本
    /// @param feed_buffer 是否将内容喂入 DisplayBuffer（流式聊天内容应为 true，
    ///                    含光标定位的 UI 元素如 StatusBar/CommandPanel 应为 false）
    void write_safe(std::string_view text, bool feed_buffer = false);

    /// @brief 启动思考计时动画
    void spinner_start(std::string_view msg);

    /// @brief 停止思考计时动画
    void spinner_stop();

    /// @brief 获取 Spinner 对象（供 ChatRenderer 设置回调）
    Spinner* get_spinner() const;

    /// @brief 设置输入回调
    void set_input_callback(InputCallback cb);

    /// @brief 设置 Tab 补全回调
    void set_completion_callback(CompletionCallback cb);

    /// @brief 设置 Ctrl+O 回调（思考视图切换）
    using CtrlOCallback = std::function<void()>;
    void set_ctrl_o_callback(CtrlOCallback cb);

    /// @brief 设置状态栏刷新回调（每次输入循环迭代后调用）
    using StatusRefreshCallback = std::function<void()>;
    void set_status_refresh_callback(StatusRefreshCallback cb);

    /// @brief 设置命令面板导航回调（↑↓ 键拦截）
    using CommandNavCallback = std::function<bool(char32_t key)>;
    void set_command_nav_callback(CommandNavCallback cb);

    /// @brief 设置命令面板 Tab 补全回调
    using CommandTabCallback = std::function<std::string()>;
    void set_command_tab_callback(CommandTabCallback cb);

    /// @brief 设置输入变化通知回调
    using InputChangedCallback = std::function<void(const std::string&)>;
    void set_input_changed_callback(InputChangedCallback cb);

    /// @brief 处理终端尺寸变更（由 LineEditor 的 resize 回调触发）
    /// @details 刷新 scroll region、重放 DisplayBuffer、发布 TerminalResizeEvent
    void handle_resize();

    /// @brief 获取历史管理器
    History& history() { return m_history; }

    /// @brief 获取平台接口（用于向导等需要直接 I/O 的场景）
    IPlatform* platform() { return m_platform.get(); }

    /// @brief 获取终端高度（行数）
    int get_terminal_height() const;

    /// @brief 获取终端宽度（列数）
    int get_terminal_width() const;

    /// @brief 直接定位光标到指定行列（绕过 save/restore，用于流结束后复位到输入行）
    void cursor_to_pos(int row, int col);

    /// @brief 开始覆盖层：快照输出区指定行范围，暂停 DisplayBuffer feed
    /// @param top_row 1-based 起始行
    /// @param bottom_row 1-based 结束行
    void begin_overlay(int top_row, int bottom_row);

    /// @brief 结束覆盖层：按行重发快照内容恢复输出区，恢复 DisplayBuffer feed
    void end_overlay();

    /// @brief 查询覆盖层是否激活（线程安全，无锁原子读）
    /// @details M-1: 统一 overlay 状态查询入口，消除 ChatRenderer 与 Terminal 间的状态非原子窗口
    bool is_overlay_active() const { return m_overlay_active.load(std::memory_order_acquire); }

    /// @brief 获取 DisplayBuffer 已记录的物理行总数
    /// @details 供 ChatRenderer 计算 "● 思考 Ns" 标记的屏幕行号，实现就地展开。
    ///          overlay 激活时行数冻结（feed 被阻止），收起后恢复。
    /// @return 物理行总数；DisplayBuffer 未初始化时返回 0
    int display_buffer_row_count() const;

    /// @brief 获取底部区域管理器
    BottomBarManager& bottom_bar() { return *m_bottom_bar; }
    const BottomBarManager& bottom_bar() const { return *m_bottom_bar; }

    // D-4/D-5/D-6：依赖解析（public，供 ChatRenderer 等同生态组件复用 DI 路径）
    /// @brief 解析事件总线（H-4：不再回退单例，调用方需保证构造时注入非空）
    agent::IEventBus& event_bus();
    /// @brief 解析配置管理器（H-4：不再回退单例）
    agent::IConfigManager& config_manager();
    /// @brief 解析任务管理器（H-4：不再回退单例）
    agent::ITaskManager& task_manager();

    /// @brief 显示欢迎横幅（WorkX logo + 模型 + 项目路径）
    /// @details 正常启动时由 run() 内部调用；会话恢复场景需显式调用以控制渲染顺序
    void display_welcome();

private:
    void run_simple();
    void run_advanced();

    /// @brief 原子化输出用户输入回显（灰底 + "> " 前缀），防止后台事件泵线程干扰
    void echo_input(const std::string& text);

    TerminalConfig m_config;
    std::unique_ptr<IPlatform> m_platform;
    std::unique_ptr<LineEditor> m_editor;
    std::unique_ptr<Spinner> m_spinner;
    std::unique_ptr<BottomBarManager> m_bottom_bar;

    InputCallback m_input_callback;
    CtrlOCallback m_ctrl_o_callback;
    StatusRefreshCallback m_status_refresh_callback;
    // T-4：跨线程访问（主线程 + 事件泵线程），原子化消除数据竞争
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_initialized{false};
    bool m_welcome_displayed = false;  ///< 欢迎横幅是否已显示（避免 run() 重复调用）

    History m_history;
    std::mutex m_output_mutex;  ///< 保护 IPlatform 输出操作

    // AskUser 跨线程协作：事件泵线程 set，主循环 take
    std::mutex m_pending_ask_mutex;
    std::optional<detail::PendingAskRequest> m_pending_ask;
    ColorRole m_current_color = ColorRole::Default;
    bool m_scroll_region_active = false;  ///< 滚动区域是否已激活
    bool m_editing = false;               ///< read_line() 是否正在运行
    bool m_cursor_in_output = true;       ///< 光标是否在输出区（滚动区域内）
    int m_edit_lines = 1;                 ///< 编辑期间输入区占用的终端行数
    std::thread m_event_pump_thread;      ///< 后台事件泵线程
    std::atomic<bool> m_event_pump_running{false};  ///< 事件泵运行标志

    std::unique_ptr<DisplayBuffer> m_display_buffer;
    // M-1: atomic 保证 ChatRenderer 后台线程查询 is_overlay_active() 时的内存可见性
    std::atomic<bool> m_overlay_active{false};
    std::vector<std::string> m_overlay_snapshot;
    int m_overlay_top = 0;
    int m_overlay_bottom = 0;

    // 最近已知的终端尺寸（用于 resize 事件发布时计算 old/new 差值）
    int m_last_width = 80;
    int m_last_height = 24;

    // D-4/D-5/D-6：DI 注入的依赖（H-4：不再回退单例，构造时必须显式注入）
    agent::IEventBus* m_event_bus = nullptr;        ///< 事件总线（非拥有）
    agent::IConfigManager* m_config_manager = nullptr;  ///< 配置管理器（非拥有）
    agent::ITaskManager* m_task_manager = nullptr;  ///< 任务管理器（非拥有）
};

} // namespace tui
