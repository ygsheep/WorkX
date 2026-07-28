/**
 * @file terminal.cpp
 * @brief Terminal 类实现
 * @version 2.0.0
 */

#include "tui/core/terminal.h"
#include "tui/input/line_editor.h"
#include "tui/render/spinner.h"
#include "tui/core/color_scheme.h"
#include "tui/core/platform/i_platform.h"
#include "tui/widgets/bottom_bar_manager.h"
#include "core/events/event_bus.h"
#include "core/config/config_manager.h"
#include "core/task/task_manager.h"
#include "agent/message/types.h"

#include <iostream>
#include <chrono>
#include <thread>
#include <filesystem>

namespace tui {

using namespace agent;  // P0: tui→agent 类型引用过渡方案，后续 P2/P3 收紧到显式前缀

// 工厂函数声明（在 platform_win32.cpp 中定义）
std::unique_ptr<IPlatform> create_platform();

Terminal::Terminal(IEventBus* event_bus,
                   IConfigManager* config_manager,
                   ITaskManager* task_manager,
                   const TerminalConfig& config)
    : m_config(config)
    , m_event_bus(event_bus)
    , m_config_manager(config_manager)
    , m_task_manager(task_manager)
{
}

Terminal::~Terminal() {
    restore();
}

// H-4：依赖解析（不再回退单例；调用方需保证构造时注入非空指针）
IEventBus& Terminal::event_bus() {
    return *m_event_bus;
}
IConfigManager& Terminal::config_manager() {
    return *m_config_manager;
}
ITaskManager& Terminal::task_manager() {
    return *m_task_manager;
}

Result<void, std::string> Terminal::initialize() {
    m_platform = create_platform();

    auto result = m_platform->enable_raw_mode();
    if (result.isErr()) {
        m_config.simple_io = true;
    }

    m_editor = std::make_unique<LineEditor>(m_platform.get());
    m_spinner = std::make_unique<Spinner>(this);
    m_bottom_bar = std::make_unique<BottomBarManager>(this);

    // 当 LineEditor 定位光标到输入行时，标记光标离开输出区
    m_editor->set_cursor_left_output_callback([this]() {
        mark_cursor_left_output();
    });

    // 当 read_line() 开始/结束时，设置 m_editing 标志
    // write() 在 m_editing 时会恢复光标到输入行，避免异步输出干扰编辑
    m_editor->set_editing_changed_callback([this](bool editing) {
        m_editing = editing;
    });

    // 加载历史文件
    // G.3：原代码在 POSIX 系统也尝试 USERPROFILE（POSIX 无此变量，会回退到 ".")
    // 修正：平台分支明确区分 Windows 和 POSIX 的家目录解析
    {
        namespace fs = std::filesystem;
        const char* home = nullptr;
#ifdef _WIN32
        home = getenv("USERPROFILE");
        if (!home || home[0] == '\0') home = getenv("APPDATA");
#else
        home = getenv("HOME");
#endif
        auto history_path = fs::path(home ? home : ".") / ".workx_history";
        m_history.load(history_path);
        m_editor->load_history(m_history.entries());
    }

    // 清屏：清除上次会话残留内容，确保从干净状态开始
    // 差分渲染会在固定行写入状态栏/输入行，退出后这些内容不会自动消失
    {
        std::lock_guard<std::mutex> lock(m_output_mutex);
        m_platform->write_output("\x1b[2J\x1b[H");
        m_platform->flush();
    }

    // 设置滚动区域，保护底部输入行和状态栏
    setup_scroll_region();

    m_display_buffer = std::make_unique<DisplayBuffer>(1000);
    m_display_buffer->set_width(get_terminal_width());
    m_display_buffer->set_height(get_terminal_height());

    // 后台事件泵线程：确保异步事件（如流式输出）能被及时派发
    // 同时驱动 TaskManager::update() 清理已完成任务（修复 1.2 / 2.1）
    m_event_pump_running = true;
    m_event_pump_thread = std::thread([this]() {
        while (m_event_pump_running) {
            event_bus().process_async_events();
            task_manager().update();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });

    m_initialized = true;
    return Result<void, std::string>::ok();
}

void Terminal::restore() {
    if (!m_initialized) return;

    // 停止后台事件泵线程
    if (m_event_pump_running) {
        m_event_pump_running = false;
        if (m_event_pump_thread.joinable()) m_event_pump_thread.join();
    }

    // 重置滚动区域
    if (m_scroll_region_active) {
        reset_scroll_region();
    }

    // 清屏：清除差分渲染残留的状态栏、输入行等内容
    // 必须在 disable_raw_mode() 之前执行，此时 VT 处理仍启用
    {
        std::lock_guard<std::mutex> lock(m_output_mutex);
        m_platform->write_output("\x1b[2J\x1b[H");
        m_platform->flush();
    }

    if (m_editor) {
        m_history.clear();
        for (const auto& entry : m_editor->get_history()) {
            m_history.add(entry);
        }

        namespace fs = std::filesystem;
        const char* home = getenv("USERPROFILE");
#ifdef _WIN32
        if (!home) home = getenv("APPDATA");
#endif
        auto history_path = fs::path(home ? home : ".") / ".workx_history";
        m_history.save(history_path);
    }

    if (m_spinner) {
        m_spinner->stop();
    }
    if (m_platform) {
        m_platform->disable_raw_mode();
    }
    m_initialized = false;
}

void Terminal::run() {
    m_running = true;

    // welcome 内容底对齐（紧挨状态栏），避免 logo 在顶部留下大量空行
    m_cursor_in_output = false;

    display_welcome();

    if (m_config.simple_io) {
        run_simple();
    } else {
        run_advanced();
    }
}

void Terminal::run_simple() {
    std::string line;
    while (m_running) {
        m_platform->write_output(m_config.prompt_string);
        m_platform->flush();

        if (!std::getline(std::cin, line)) {
            break;
        }

        if (line.empty()) continue;

        if (line[0] == '/') {
            // 内置命令直接处理
            std::string cmd_text = line.substr(1);
            auto space_pos = cmd_text.find(' ');
            std::string cmd_name = (space_pos != std::string::npos)
                ? cmd_text.substr(0, space_pos) : cmd_text;

            if (cmd_name == "exit" || cmd_name == "quit") {
                break;
            }
        } else {
            echo_input(line);
        }

        event_bus().publish(UserInputEvent{
            .text = line
        });
        event_bus().process_async_events();

        if (m_input_callback) {
            m_input_callback(line);
        }
    }
}

void Terminal::echo_input(const std::string& text) {
    std::lock_guard<std::mutex> lock(m_output_mutex);

    int height = m_platform->get_terminal_height();
    int scroll_bottom = height - 3;
    if (scroll_bottom < 1) scroll_bottom = 1;

    char cmd[32];
    snprintf(cmd, sizeof(cmd), "\x1b[%d;1H", scroll_bottom);

    m_platform->write_output("\x1b[0m");
    m_platform->write_output(get_color_ansi(ColorRole::UserInput));
    m_platform->write_output(cmd);
    m_platform->write_output("> ");
    m_platform->write_output(text);
    // 用当前背景色填充到行尾（EL 序列 \x1b[K 应用当前 SGR 背景色）
    // 实现整行灰色背景，而不是只有文字部分有背景色
    m_platform->write_output("\x1b[K");
    // 先清除背景色再换行，避免 \r\n 时背景色延伸到下一行（trailing background）
    m_platform->write_output("\x1b[0m");
    m_platform->write_output("\r\n");
    m_platform->flush();

    m_current_color = ColorRole::Default;
    m_cursor_in_output = true;

    if (m_display_buffer && !m_overlay_active.load(std::memory_order_acquire)) {
        m_display_buffer->feed("\x1b[0m");
        m_display_buffer->feed(get_color_ansi(ColorRole::UserInput));
        m_display_buffer->feed(cmd);
        m_display_buffer->feed("> ");
        m_display_buffer->feed(text);
        m_display_buffer->feed("\x1b[K");
        m_display_buffer->feed("\x1b[0m");
        m_display_buffer->feed("\n");
    }
}

void Terminal::run_advanced() {
    using clock = std::chrono::steady_clock;
    auto last_interrupt_time = clock::time_point{};
    static constexpr auto DOUBLE_PRESS_TIMEOUT = std::chrono::milliseconds(1000);

    while (m_running) {
        event_bus().process_async_events();

        // 刷新底部区域（状态栏或命令面板）
        if (m_status_refresh_callback) {
            m_status_refresh_callback();
        }
        // 也刷新 BottomBarManager（它决定渲染哪个面板）
        if (m_bottom_bar) {
            m_bottom_bar->render();
        }

        auto result = m_editor->read_line(m_config.prompt_string);

        // Enter 不触发 input_changed 回调，命令面板可能仍然可见，需先 dismiss
        if (m_bottom_bar && m_bottom_bar->mode() == BottomBarMode::COMMAND_PANEL) {
            m_bottom_bar->set_mode(BottomBarMode::STATUS_BAR);
        }

        if (result.stream_end) {
            m_running = false;
            break;
        }

        if (result.interrupted) {
            auto now = clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_interrupt_time);

            if (elapsed < DOUBLE_PRESS_TIMEOUT) {
                set_color(ColorRole::System);
                write("Force exit.\n");
                reset_color();
                event_bus().publish(InterruptEvent{.force = true});
                event_bus().publish(ShutdownEvent{.force = true});
                m_running = false;
                break;
            }

            last_interrupt_time = now;
            set_color(ColorRole::System);
            write("(Press Ctrl+C again to exit)\n");
            reset_color();
            event_bus().publish(InterruptEvent{.force = false});
            event_bus().process_async_events();
            continue;
        }

        // Ctrl+O 切换思考视图
        if (result.ctrl_o) {
            if (m_ctrl_o_callback) {
                m_ctrl_o_callback();
            }
            continue;
        }

        if (result.text.empty()) continue;

        if (result.is_command) {
            // 内置命令直接处理
            std::string cmd_text = result.text.substr(1);
            auto space_pos = cmd_text.find(' ');
            std::string cmd_name = (space_pos != std::string::npos)
                ? cmd_text.substr(0, space_pos) : cmd_text;

            if (cmd_name == "exit" || cmd_name == "quit") {
                m_running = false;
                break;
            }
            if (cmd_name == "clear") {
                write("\x1b[2J\x1b[H");
                continue;
            }
        } else {
            echo_input(result.text);
        }

        event_bus().publish(UserInputEvent{
            .text = result.text
        });

        if (m_input_callback) {
            m_input_callback(result.text);
        }
    }
}

void Terminal::shutdown() {
    m_running = false;
}

void Terminal::set_color(ColorRole role) {
    if (!m_config.use_color) return;
    if (m_current_color == role) return;

    std::lock_guard<std::mutex> lock(m_output_mutex);
    // 先清除所有属性再设置新色，防止背景/加粗等属性跨线程残留
    m_platform->write_output("\x1b[0m");
    m_platform->write_output(get_color_ansi(role));
    m_current_color = role;
    if (m_display_buffer && !m_overlay_active.load(std::memory_order_acquire)) {
        m_display_buffer->feed("\x1b[0m");
        m_display_buffer->feed(std::string(get_color_ansi(role)));
    }
}

void Terminal::reset_color() {
    if (!m_config.use_color) return;
    if (m_current_color == ColorRole::Default) return;

    std::lock_guard<std::mutex> lock(m_output_mutex);
    m_platform->write_output(get_color_ansi(ColorRole::Default));
    m_current_color = ColorRole::Default;
    if (m_display_buffer && !m_overlay_active.load(std::memory_order_acquire)) {
        m_display_buffer->feed("\x1b[0m");
    }
}

void Terminal::write(std::string_view text) {
    std::lock_guard<std::mutex> lock(m_output_mutex);
    if (m_scroll_region_active) {
        if (!m_cursor_in_output) {
            // 光标不在输出区：归位到滚动区域底部再写入
            int height = m_platform->get_terminal_height();
            int scroll_bottom = height - 3;
            if (scroll_bottom < 1) scroll_bottom = 1;
            char cmd[32];
            snprintf(cmd, sizeof(cmd), "\x1b[%d;1H", scroll_bottom);
            m_platform->write_output(cmd);
            m_cursor_in_output = true;
        }
        // 光标在输出区，直接写入
        m_platform->write_output(text);
        m_platform->flush();
    } else {
        m_platform->write_output(text);
        m_platform->flush();
    }
    if (m_display_buffer && !m_overlay_active.load(std::memory_order_acquire)) {
        m_display_buffer->feed(text);
    }
}

void Terminal::write_safe(std::string_view text, bool feed_buffer) {
    std::lock_guard<std::mutex> lock(m_output_mutex);
    // 统一 save/restore cursor，防止跨滚动区域边界 restore 失效
    m_platform->write_output("\x1b" "7");  // DECSC (VT100 标准，兼容性优于 \x1b[s)
    m_platform->write_output(text);
    m_platform->write_output("\x1b" "8");  // DECRC (VT100 标准，兼容性优于 \x1b[u)
    m_platform->flush();
    if (feed_buffer && m_display_buffer && !m_overlay_active.load(std::memory_order_acquire)) {
        m_display_buffer->feed(text);
    }
}

void Terminal::cursor_to_pos(int row, int col) {
    std::lock_guard<std::mutex> lock(m_output_mutex);
    m_platform->write_output("\x1b[" + std::to_string(row) + ";" + std::to_string(col) + "H");
    m_platform->flush();
}

void Terminal::move_cursor_safe(int cols) {
    std::lock_guard<std::mutex> lock(m_output_mutex);
    m_platform->move_cursor(cols);
    m_platform->flush();
}

void Terminal::spinner_start(std::string_view msg) {
    if (m_spinner) {
        m_spinner->start(msg);
    }
}

void Terminal::spinner_stop() {
    if (m_spinner) {
        m_spinner->stop();
    }
}

Spinner* Terminal::get_spinner() const {
    return m_spinner.get();
}

int Terminal::get_terminal_height() const {
    if (m_platform) return m_platform->get_terminal_height();
    return 24;
}

int Terminal::get_terminal_width() const {
    if (m_platform) return m_platform->get_terminal_width();
    return 80;
}

void Terminal::setup_scroll_region() {
    int height = get_terminal_height();
    // 滚动区域: 行 1 ~ height-3（留出底部 3 行给 StatusBar + 输入行 + CommandPanel）
    // ANSI: \x1b[top;bottomr  (1-based)
    int scroll_bottom = height - 3;
    if (scroll_bottom < 1) scroll_bottom = 1;

    std::lock_guard<std::mutex> lock(m_output_mutex);
    // 设置滚动区域
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "\x1b[1;%dr", scroll_bottom);
    m_platform->write_output(cmd);
    // 将光标定位到滚动区域顶部，确保后续输出从顶部开始
    m_platform->write_output("\x1b[1;1H");
    m_platform->flush();
    m_scroll_region_active = true;
    m_cursor_in_output = true;
}

void Terminal::reset_scroll_region() {
    std::lock_guard<std::mutex> lock(m_output_mutex);
    // 重置为全屏滚动
    m_platform->write_output("\x1b[r");
    m_platform->write_output("\x1b[H");  // 光标归位
    m_platform->flush();
    m_scroll_region_active = false;
}

void Terminal::cursor_to_output() {
    int height = get_terminal_height();
    int scroll_bottom = height - 3;
    if (scroll_bottom < 1) scroll_bottom = 1;

    std::lock_guard<std::mutex> lock(m_output_mutex);
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "\x1b[%d;1H", scroll_bottom);
    m_platform->write_output(cmd);
    m_platform->flush();
    m_cursor_in_output = true;
}

void Terminal::mark_cursor_left_output() {
    std::lock_guard<std::mutex> lock(m_output_mutex);
    m_cursor_in_output = false;
}

void Terminal::set_input_callback(InputCallback cb) {
    m_input_callback = std::move(cb);
}

void Terminal::set_ctrl_o_callback(CtrlOCallback cb) {
    m_ctrl_o_callback = std::move(cb);
}


void Terminal::set_status_refresh_callback(StatusRefreshCallback cb) {
    m_status_refresh_callback = std::move(cb);
}

void Terminal::set_command_nav_callback(CommandNavCallback cb) {
    if (m_editor) {
        m_editor->set_command_nav_callback(std::move(cb));
    }
}

void Terminal::set_command_tab_callback(CommandTabCallback cb) {
    if (m_editor) {
        m_editor->set_command_tab_callback(std::move(cb));
    }
}

void Terminal::set_input_changed_callback(InputChangedCallback cb) {
    if (m_editor) {
        m_editor->set_input_changed_callback(std::move(cb));
    }
}

void Terminal::set_completion_callback(CompletionCallback cb) {
    if (m_editor) {
        m_editor->set_completion_callback([c = std::move(cb)](std::string_view line, size_t pos)
            -> std::vector<CompletionCandidate> {
            auto raw = c(line, pos);
            std::vector<CompletionCandidate> result;
            result.reserve(raw.size());
            for (auto& [text, cursor] : raw) {
                result.push_back({std::move(text), cursor});
            }
            return result;
        });
    }
}

void Terminal::begin_overlay(int top_row, int bottom_row) {
    std::lock_guard<std::mutex> lock(m_output_mutex);
    if (m_overlay_active.load(std::memory_order_acquire) || !m_display_buffer) return;
    m_overlay_top = top_row;
    m_overlay_bottom = bottom_row;
    m_overlay_snapshot = m_display_buffer->snapshot(top_row, bottom_row);
    m_overlay_active.store(true, std::memory_order_release);
}

void Terminal::end_overlay() {
    std::lock_guard<std::mutex> lock(m_output_mutex);
    if (!m_overlay_active.load(std::memory_order_acquire)) return;
    m_overlay_active.store(false, std::memory_order_release);

    // 保存当前光标位置，恢复覆盖层内容后还原
    m_platform->write_output("\x1b" "7");  // DECSC (VT100 标准，兼容性优于 \x1b[s)

    for (int r = m_overlay_top; r <= m_overlay_bottom; ++r) {
        int idx = r - m_overlay_top;
        char pos_cmd[32];
        snprintf(pos_cmd, sizeof(pos_cmd), "\x1b[%d;1H", r);
        m_platform->write_output(pos_cmd);
        m_platform->write_output("\x1b[2K");
        if (idx < static_cast<int>(m_overlay_snapshot.size())
            && !m_overlay_snapshot[idx].empty()) {
            m_platform->write_output(m_overlay_snapshot[idx]);
        }
    }
    m_platform->flush();
    m_overlay_snapshot.clear();

    // 恢复光标到覆盖层恢复前的位置
    m_platform->write_output("\x1b" "8");  // DECRC (VT100 标准，兼容性优于 \x1b[u)
    m_platform->flush();
    m_cursor_in_output = false;
}

void Terminal::display_welcome() {
    int width = m_platform->get_terminal_width();

    std::string version_str = "0.2.0";
#ifdef WORKX_VERSION
    version_str = WORKX_VERSION;
#endif

    if (width >= 60) {
        std::string ver_line = "WorkX v" + version_str;
        std::string model_name = config_manager().get_or<std::string>(
            "backend.model_name", "");
        std::string model_line = (model_name.empty() ? std::string("unknown") : model_name)
            + " \xc2\xb7 API Usage Billing";
        namespace fs = std::filesystem;
        std::string cwd = fs::current_path().string();

        auto render_line = [&](const char* logo, const char* info, ColorRole info_color) {
            set_color(ColorRole::TextColor);
            write(logo);
            if (info && info[0] != '\0') {
                set_color(info_color);
                write(info);
            }
            write("\r\n");
            reset_color();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        };

        // WorkX — ANSI Shadow 字符画（6 行）
        render_line(
            "██╗    ██╗ ██████╗ ██████╗ ██╗  ██╗██╗  ██╗    ",
            ver_line.c_str(), ColorRole::SplashInfo);

        render_line(
            " ██║    ██║██╔═══██╗██╔══██╗██║ ██╔╝╚██╗██╔╝    ",
            model_line.c_str(), ColorRole::SplashInfo);

        render_line(
            " ██║ █╗ ██║██║   ██║██████╔╝█████╔╝  ╚███╔╝    ",
            cwd.c_str(), ColorRole::Dim);

        render_line(
            " ██║███╗██║██║   ██║██╔══██╗██╔═██╗  ██╔██╗    ",
            nullptr, ColorRole::Default);

        render_line(
            " ╚███╔███╔╝╚██████╔╝██║  ██║██║  ██╗██╔╝ ██╗    ",
            "/help for commands", ColorRole::Dim);

        render_line(
            "  ╚══╝╚══╝  ╚═════╝ ╚═╝  ╚═╝╚═╝  ╚═╝╚═╝  ╚═╝    ",
            nullptr, ColorRole::Default);
    } else {
        std::string model_name = config_manager().get_or<std::string>(
            "backend.model_name", "");
        set_color(ColorRole::SplashInfo);
        write("[WorkX] v");
        write(version_str);
        write("\n" + (model_name.empty() ? std::string("unknown") : model_name)
              + "\n/help for commands\n");
        reset_color();
    }

    write("\r\n");
}

} // namespace tui
