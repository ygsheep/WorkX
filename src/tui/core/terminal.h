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

#include "core/utils/result.h"
#include "tui/core/color_scheme.h"
#include "tui/input/history.h"
#include "tui/core/display_buffer.h"
#include "core/events/i_event_bus.h"
#include "core/config/i_config_manager.h"
#include "core/task/task_manager.h"  // ITaskManager + TaskManager::instance() 默认实参

namespace agent {

class IPlatform;
class LineEditor;
class Spinner;
class StatusBar;
class ChatRenderer;
class BottomBarManager;

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
    /// @param config 终端配置
    /// @param event_bus 事件总线（nullptr 时回退 EventBus::instance()，D-4 DI 化）
    /// @param config_manager 配置管理器（nullptr 时回退 ConfigManager::instance()，D-5 DI 化）
    /// @param task_manager 任务管理器（nullptr 时回退 TaskManager::instance()，D-6 DI 化）
    explicit Terminal(const TerminalConfig& config = TerminalConfig{},
                      IEventBus* event_bus = nullptr,
                      IConfigManager* config_manager = nullptr,
                      ITaskManager* task_manager = nullptr);
    ~Terminal();

    /// @brief 初始化终端（raw mode、平台设置、滚动区域）
    Result<void, std::string> initialize();

    /// @brief 设置 ANSI 滚动区域，保护底部输入行和状态栏
    void setup_scroll_region();

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

    /// @brief 获取底部区域管理器
    BottomBarManager& bottom_bar() { return *m_bottom_bar; }
    const BottomBarManager& bottom_bar() const { return *m_bottom_bar; }

    // D-4/D-5/D-6：依赖解析（public，供 ChatRenderer 等同生态组件复用 DI 路径）
    /// @brief 解析事件总线（nullptr 时回退单例）
    IEventBus& event_bus();
    /// @brief 解析配置管理器（nullptr 时回退单例）
    IConfigManager& config_manager();
    /// @brief 解析任务管理器（nullptr 时回退单例）
    ITaskManager& task_manager();

private:
    void display_welcome();
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

    History m_history;
    std::mutex m_output_mutex;  ///< 保护 IPlatform 输出操作
    ColorRole m_current_color = ColorRole::Default;
    bool m_scroll_region_active = false;  ///< 滚动区域是否已激活
    bool m_editing = false;               ///< read_line() 是否正在运行
    bool m_cursor_in_output = true;       ///< 光标是否在输出区（滚动区域内）
    std::thread m_event_pump_thread;      ///< 后台事件泵线程
    std::atomic<bool> m_event_pump_running{false};  ///< 事件泵运行标志

    std::unique_ptr<DisplayBuffer> m_display_buffer;
    bool m_overlay_active = false;
    std::vector<std::string> m_overlay_snapshot;
    int m_overlay_top = 0;
    int m_overlay_bottom = 0;

    // D-4/D-5/D-6：DI 注入的依赖（nullptr 时回退单例，向后兼容）
    IEventBus* m_event_bus = nullptr;        ///< 事件总线（非拥有）
    IConfigManager* m_config_manager = nullptr;  ///< 配置管理器（非拥有）
    ITaskManager* m_task_manager = nullptr;  ///< 任务管理器（非拥有）
};

} // namespace agent
