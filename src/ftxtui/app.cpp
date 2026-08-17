#include "app.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#undef RGB
#endif

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/direction.hpp>
#include <ftxui/screen/terminal.hpp>

#include "agent/api/i_backend_admin.h"
#include "agent/api/chat_types.h"
#include "agent/command/inclaude/registry.h"
#include "agent/core/chat_session.h"
#include "agent/session/session_store.h"
#include "command/command_registry.h"
#include "core/events/stream_events.h"
#include "theme/theme.h"
#include "render/markdown_to_elements.h"
#include "widgets/sidebar.h"
#include "widgets/status_line.h"
#include "widgets/composer.h"
#include "widgets/command_palette.h"
#include "widgets/model_selector.h"

namespace ftxtui {

using ftxui::Color;
using ftxui::Element;
using ftxui::Elements;
using ftxui::Event;

namespace {
/// @brief 输入区固定高度（行数）
constexpr int kComposerHeight = 3;
/// @brief 侧栏折叠宽度阈值：低于该列宽时不渲染右侧栏（窄屏保护内容区）
constexpr int kSidebarCollapseWidth = 100;

#if defined(_WIN32)

/// @brief 兜底检测真实控制台视口尺寸。
/// FTXUI 的 Terminal::Size() 依赖 GetStdHandle(STD_OUTPUT_HANDLE)，
/// 当 stdout 是管道（如 opencode 终端）时 GetConsoleScreenBufferInfo 失败并回退 80x24，
/// 导致渲染宽度小于实际终端宽度、右侧露出终端原背景。此处改用 CONOUT$ 打开真实控制台。
bool detect_console_size(int& w, int& h) {
    HANDLE h_out = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi = {};
    bool ok = h_out != nullptr && h_out != INVALID_HANDLE_VALUE &&
              GetConsoleScreenBufferInfo(h_out, &csbi);
    if (!ok) {
        // stdout 是管道（如 opencode 终端）时 GetConsoleScreenBufferInfo 失败，
        // 改用 CONOUT$ 打开真实控制台（ConPTY 下同样可用）。
        h_out = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, 0, nullptr);
        if (h_out == INVALID_HANDLE_VALUE) return false;
        ok = GetConsoleScreenBufferInfo(h_out, &csbi);
        if (!ok) return false;
    }
    w = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    h = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    return true;
}
#endif

/// @brief 生成 mock 思考内容（思考折叠块演示，随用户消息变化）
std::string build_mock_reasoning(const std::string& user_text) {
    std::string r;
    r += "收到消息：「" + user_text + "」\n";
    r += "1. 解析用户意图，识别关键词与隐含诉求\n";
    r += "2. 若涉及文件/命令操作，规划工具调用顺序\n";
    r += "3. 组织回答结构：结论优先，附代码示例\n";
    r += "4. 检查边界条件：空输入、超长文本、特殊字符\n";
    return r;
}

/// @brief 生成 mock LLM 回复（Markdown 渲染演示）
std::string build_mock_reply(const std::string& user_text) {
    std::string reply;
    reply += "### 收到你的消息\n\n";
    reply += "你说的是：「" + user_text + "」\n\n";
    reply += "**Markdown 语法演示：**\n\n";
    reply += "1. 有序列表\n";
    reply += "2. **加粗**、*斜体*、~~删除线~~\n";
    reply += "3. 行内代码 `std::string`\n\n";
    reply += "---\n\n";
    reply += "```cpp\n";
    reply += "#include <iostream>\n";
    reply += "int main() {\n";
    reply += "    std::cout << \"hello workx\" << std::endl;\n";
    reply += "    return 0;\n";
    reply += "}\n";
    reply += "```\n\n";
    reply += "这是实验 TUI 的 mock 回复，用于验证 Markdown 渲染效果。\n";
    return reply;
}
}  // namespace

App::App(AppDeps deps)
    : m_deps(std::move(deps)),
      m_bridge(*m_deps.event_bus, m_queue),
      m_screen(ftxui::ScreenInteractive::Fullscreen()) {
    m_vm.sidebar.model = m_deps.model_name;
    m_vm.sidebar.project = m_deps.project;
    m_vm.sidebar.agent = m_deps.agent_name;

    m_bridge.set_wake_callback([this] { m_screen.PostEvent(Event::Custom); });
    m_bridge.start();
}

App::~App() {
    m_anim_run = false;
    if (m_anim_thread.joinable()) m_anim_thread.join();
    m_stream_run = false;
    if (m_stream_thread.joinable()) m_stream_thread.join();
    m_bridge.stop();
}

// ---------------------------------------------------------------------------
// 每帧事件处理
// ---------------------------------------------------------------------------

void App::handle_ask_user(const ActionAskUser& a) {
    m_ask_promise = a.result_promise;
    m_ask_question = "（请作答）";
    if (a.questions.is_array() && !a.questions.empty()) {
        const auto& q = a.questions[0];
        if (q.contains("question") && q["question"].is_string())
            m_ask_question = q["question"].get<std::string>();
        else if (q.contains("header") && q["header"].is_string())
            m_ask_question = q["header"].get<std::string>();
    }
    m_ask_active = true;
    m_ask_buffer.clear();
    m_ask_deadline = a.timeout_ms > 0
        ? std::chrono::steady_clock::now() + std::chrono::milliseconds(a.timeout_ms)
        : std::chrono::steady_clock::time_point::max();
    if (m_ask_input) m_ask_input->TakeFocus();
}

void App::close_ask(bool submitted) {
    if (m_ask_promise) {
        agent::AskUserResult r;
        r.submitted = submitted;
        if (submitted) {
            std::string ans = m_ask_buffer;
            size_t b = ans.find_first_not_of(" \t");
            size_t e = ans.find_last_not_of(" \t");
            if (b == std::string::npos) ans.clear();
            else ans = ans.substr(b, e - b + 1);
            r.answers.emplace_back(m_ask_question, std::move(ans));
        }
        m_ask_promise->set_value(std::move(r));
        m_ask_promise.reset();
    }
    m_ask_active = false;
    m_ask_buffer.clear();
}

void App::drain() {
    // 兜底派发异步事件（EventBus 的 M-8 契约：无事件泵线程时必须显式 drain）
    // AskUserRequestEvent / StreamErrorEvent 等经 publish_async 入队，依赖此处派发
    if (m_deps.event_bus) m_deps.event_bus->process_async_events();

    auto actions = m_queue.drain();
    bool changed = false;
    for (auto& a : actions) {
        try {
            if (auto* ask = std::get_if<ActionAskUser>(&a)) {
                handle_ask_user(*ask);
                changed = true;
                continue;
            }
            if (auto* to = std::get_if<ActionAskUserTimeout>(&a)) {
                if (m_ask_active) close_ask(false);
                changed = true;
                continue;
            }
            if (auto* toast = std::get_if<ActionToast>(&a)) {
                m_vm.prompt_echo = toast->text;
                changed = true;
                continue;
            }
            if (auto* models = std::get_if<ActionModelsLoaded>(&a)) {
                m_model_items = models->models;
                changed = true;
                continue;
            }
            changed |= m_vm.apply(a);
        } catch (const std::exception& ex) {
            log_run(std::string("drain: action exception: ") + ex.what());
        }
    }
    // 同步忙标志（供动画线程读取，避免跨线程读 ViewModel）
    m_busy = m_vm.busy;
    if (changed) m_screen.RequestAnimationFrame();
    if (m_vm.pending_exit) m_screen.Exit();
}

void App::log_run(std::string_view msg) {
    std::lock_guard<std::mutex> lock(m_log_mutex);
    FILE* f = fopen("D:\\develop\\Workspace\\workx\\codex_run.log", "a");
    if (!f) return;
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_s(&tm, &t);
    char ts[64];
    std::strftime(ts, sizeof(ts), "%H:%M:%S", &tm);
    fprintf(f, "[%s] %.*s\n", ts, static_cast<int>(msg.size()), msg.data());
    fclose(f);
}

// ---------------------------------------------------------------------------
// 模型选择
// ---------------------------------------------------------------------------

void App::open_model_selector() {
    if (!m_deps.backend_admin) {
        m_vm.apply(ActionAppendMessage{.role = "assistant",
            .text = "（无后端，无法列出模型）\n"});
        return;
    }
    m_model_items.clear();
    m_model_open = true;
    // 后台拉取模型列表，完成后入队 action 刷新（避免阻塞 UI）
    std::thread([this]() {
        auto result = m_deps.backend_admin->list_models();
        std::vector<std::string> names;
        if (result.is_ok()) {
            for (const auto& m : result.value())
                names.push_back(m.name);
        } else {
            names.push_back("（模型列表获取失败）");
        }
        m_queue.push(ActionModelsLoaded{.models = std::move(names)});
        m_screen.PostEvent(Event::Custom);
    }).detach();
}

void App::apply_model(int index) {
    if (index < 0 || index >= static_cast<int>(m_model_items.size())) return;
    const std::string& name = m_model_items[static_cast<size_t>(index)];
    if (name.rfind("（", 0) == 0) return;  // 占位错误行，忽略
    if (m_deps.backend_admin) m_deps.backend_admin->set_model_name(name);
    m_vm.sidebar.model = name;
    if (m_deps.on_model_changed) m_deps.on_model_changed();
}

void App::cmd_resume(const std::string& args) {
    if (!m_deps.session || m_deps.session_dir.empty()) {
        m_vm.apply(ActionAppendMessage{.role = "assistant",
            .text = "（无会话后端，无法恢复历史会话）\n"});
        return;
    }
    auto sessions = agent::session::SessionStore::list_sessions(m_deps.session_dir);
    if (sessions.empty()) {
        m_vm.apply(ActionAppendMessage{.role = "assistant",
            .text = "（当前项目暂无历史会话）\n"});
        return;
    }

    if (args.empty()) {
        // 无参：列出最近 5 条
        std::string list = "历史会话（`/resume <编号>` 恢复）：\n";
        const int n = static_cast<int>(std::min<size_t>(5, sessions.size()));
        for (int i = 0; i < n; ++i) {
            const auto& s = sessions[static_cast<size_t>(i)];
            std::string title = s.title.empty() ? s.session_id : s.title;
            list += std::to_string(i + 1) + ". " + title
                    + "（" + std::to_string(s.message_count) + " 条消息）\n";
        }
        m_vm.apply(ActionAppendMessage{.role = "assistant", .text = list});
        return;
    }

    // 带编号：恢复对应会话
    int idx = 0;
    try { idx = std::stoi(args); } catch (...) { idx = 0; }
    if (idx < 1 || idx > static_cast<int>(sessions.size())) {
        m_vm.apply(ActionAppendMessage{.role = "assistant",
            .text = "（编号超出范围，`/resume` 查看列表）\n"});
        return;
    }
    const auto& meta = sessions[static_cast<size_t>(idx - 1)];
    if (!m_deps.session->switch_session(meta.file_path)) {
        m_vm.apply(ActionAppendMessage{.role = "assistant",
            .text = "（会话切换失败：" + meta.file_path + "）\n"});
        return;
    }

    // 载入历史消息（user/assistant 文本；tool 消息并入上一条 assistant 摘要）
    m_vm.messages.clear();
    for (const auto& cm : m_deps.session->get_messages()) {
        if (cm.role == agent::ChatMessage::Role::User) {
            m_vm.apply(ActionAppendMessage{.role = "user", .text = cm.content});
        } else if (cm.role == agent::ChatMessage::Role::Assistant) {
            std::string text = cm.content;
            if (!cm.tool_uses.empty())
                text += "\n\n[工具调用 ×" + std::to_string(cm.tool_uses.size()) + "]";
            m_vm.apply(ActionAppendMessage{.role = "assistant", .text = text});
        } else if (cm.role == agent::ChatMessage::Role::Tool && !m_vm.messages.empty()) {
            auto& back = m_vm.messages.back();
            if (back.role == MsgRole::Assistant) {
                back.text += "\n> tool: " + cm.tool_name + " → "
                             + (cm.is_error ? "✗ " : "✓ ")
                             + cm.content.substr(0, std::min<size_t>(80, cm.content.size()));
            }
        }
    }
    m_scroll = 0;
    m_follow = true;
    std::string title = meta.title.empty() ? meta.session_id : meta.title;
    m_vm.sidebar.title = title;
    m_vm.apply(ActionAppendMessage{.role = "assistant",
        .text = "已恢复会话：**" + title + "**\n"});
}

void App::cmd_rename(const std::string& args) {
    if (args.empty()) {
        m_vm.apply(ActionAppendMessage{.role = "assistant",
            .text = "用法：`/rename <新标题>`\n"});
        return;
    }
    auto store = m_deps.session ? m_deps.session->session_store() : nullptr;
    if (store) store->append_title(args);
    m_vm.sidebar.title = args;
    m_vm.apply(ActionAppendMessage{.role = "assistant",
        .text = "会话标题已更新：**" + args + "**\n"});
}

void App::send_input(const std::string& text) {
    // 本地命令：不发送给模型
    if (!text.empty() && text[0] == '/') {
        std::string cmd = text;
        std::string args;
        auto sp = cmd.find(' ');
        if (sp != std::string::npos) {
            args = cmd.substr(sp + 1);
            cmd = cmd.substr(0, sp);
        }
        if (cmd == "/exit") {
            m_vm.apply(ActionShutdown{});
            if (m_vm.pending_exit) m_screen.Exit();
            return;
        }
        if (cmd == "/model") {
            open_model_selector();
            return;
        }
        if (cmd == "/help") {
            m_vm.apply(ActionAppendMessage{.role = "assistant",
                .text = "可用命令：/help /clear /exit /model /resume /rename\n"
                        "快捷键：Enter 发送 · Ctrl+P 命令面板 · Shift+Tab 权限 · Ctrl+O 思考"});
            return;
        }
        if (cmd == "/clear") {
            if (m_deps.session) m_deps.session->clear_history();
            m_vm.messages.clear();
            m_scroll = 0;
            return;
        }
        if (cmd == "/resume") {
            cmd_resume(args);
            return;
        }
        if (cmd == "/rename") {
            cmd_rename(args);
            return;
        }
        // 其余本地命令：交给 InputProcessor 管线执行（命令面板注册的命令）
        if (m_deps.event_bus && !m_deps.mock_mode) {
            m_deps.event_bus->publish(agent::UserInputEvent{.text = text});
            return;
        }
        // 其余本地命令暂以回显提示处理
        m_vm.apply(ActionAppendMessage{.role = "assistant",
                                       .text = "（实验 TUI 暂未实现该本地命令）\n"});
        return;
    }

    // 回显用户消息
    m_vm.apply(ActionAppendMessage{.role = "user", .text = text});
    m_vm.apply(ActionSetBusy{.busy = true});
    m_follow = true;

    if (m_deps.mock_mode) {
        start_mock_stream(text);
        return;
    }

    // 真实链路：发布 UserInputEvent → main 的 InputProcessor 管线 → ChatSession
    if (m_deps.event_bus) {
        m_deps.event_bus->publish(agent::UserInputEvent{.text = text});
    } else {
        m_vm.apply(ActionSetBusy{.busy = false});
    }
}

void App::start_mock_stream(const std::string& user_text) {
    // 清理可能残留的旧流
    if (m_stream_thread.joinable()) {
        m_stream_run = false;
        m_stream_thread.join();
    }

    const std::string reply = build_mock_reply(user_text);
    const std::string reasoning = build_mock_reasoning(user_text);
    log_run("mock: stream start reply_len=" + std::to_string(reply.size()) +
            " reasoning_len=" + std::to_string(reasoning.size()));
    m_stream_run = true;
    m_stream_thread = std::thread([this, reply, reasoning] {
        // 思考阶段：推送 reasoning 增量（思考中动画由 UI 帧驱动），计时用于折叠标签
        const auto t0 = std::chrono::steady_clock::now();
        constexpr size_t kThinkChunk = 6;
        for (size_t i = 0; i < reasoning.size() && m_stream_run; i += kThinkChunk) {
            m_queue.push(ActionReasoningDelta{.delta = reasoning.substr(i, kThinkChunk)});
            m_screen.PostEvent(Event::Custom);
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        const auto think_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        log_run("mock: reasoning done think_ms=" + std::to_string(think_ms));
        // 工具调用演示：read_file + grep（模拟 ReAct 单步）
        const auto push_tool = [this](const std::string& name, ActionBeginTool begin,
                                      ActionEndTool end, int run_ms) {
            log_run("mock: tool begin " + name);
            m_queue.push(std::move(begin));
            m_screen.PostEvent(Event::Custom);
            std::this_thread::sleep_for(std::chrono::milliseconds(run_ms));
            m_queue.push(std::move(end));
            m_screen.PostEvent(Event::Custom);
            log_run("mock: tool end " + name);
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        };
        push_tool("read_file",
            ActionBeginTool{.tool_name = "read_file",
                            .call_id = "mock-read",
                            .arguments = "{\"path\": \"README.md\"}"},
            ActionEndTool{.call_id = "mock-read",
                          .result = "```text\n# WorkX\n\nFTXUI 实验 TUI：\n"
                                    "- 折叠卡片（思考 / 工具）\n"
                                    "- 点击头部展开 / 收起\n"
                                    "- Nerd Font 图标\n```\n"},
            900);
        push_tool("grep",
            ActionBeginTool{.tool_name = "grep",
                            .call_id = "mock-grep",
                            .arguments = "{\"pattern\": \"TODO\", \"path\": \"src/\"}"},
            ActionEndTool{.call_id = "mock-grep",
                          .result = "src/app.cpp:12:  // TODO: 待确认\n"
                                    "src/tui/main.cpp:30:  // TODO: 侧栏折叠\n"},
            700);
        // 正文阶段：每 tick 推送一小段，模拟 LLM 流式输出
        constexpr size_t kChunk = 3;
        for (size_t i = 0; i < reply.size() && m_stream_run; i += kChunk) {
            m_queue.push(ActionTokenDelta{.content_delta = reply.substr(i, kChunk)});
            m_screen.PostEvent(Event::Custom);
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        // 结束：封口并回到 IDLE（prompt_ms 带思考耗时）
        m_queue.push(ActionTurnDone{.full_content = reply, .prompt_ms = think_ms});
        m_screen.PostEvent(Event::Custom);
        log_run("mock: turn done");
        m_stream_run = false;
        log_run("mock: stream exit");
    });
}

// ---------------------------------------------------------------------------
// 布局行数（与 build_message 渲染逐行对齐，供滚动估算与点击命中共用）
// ---------------------------------------------------------------------------

int App::layout_rows(const MessageNode& m, int y_base) {
    auto count_lines = [](std::string_view s) {
        int n = 1;
        for (char c : s)
            if (c == '\n') ++n;
        return n;
    };
    int y = y_base;
    if (m.role == MsgRole::User) {
        y += 2;  // 顶部/底部留白各 1 行
        if (!m.text.empty() || m.streaming)
            y += count_lines(m.text);
    } else {
        if (m.role == MsgRole::Error)
            ++y;  // "✖ 错误" 头
        if (m.reasoned && !m.reasoning.empty()) {
            y += 2;  // 卡片上/下边框
            if (m.reasoning_expanded)
                y += count_lines(m.reasoning);
        }
        if (!m.text.empty() || m.streaming)
            y += count_lines(m.text);
        for (const auto& t : m.tool_calls) {
            y += 2;  // 卡片上/下边框
            if (t.done && t.expanded)
                y += count_lines(t.result);
        }
        if (m.streaming) ++y;  // 流式游标
    }
    return y - y_base;
}

int App::approx_height(int width) const {
    (void)width;
    int h = 0;
    for (const auto& m : m_vm.messages) {
        h += layout_rows(m, 0);
        h += 1;  // 消息间距
    }
    return h;
}

Element App::build_transcript(int width) {
    Elements es;
    m_hits.clear();
    int y = 0;  // 内容坐标（与渲染逐行对齐）
    for (std::size_t i = 0; i < m_vm.messages.size(); ++i) {
        const auto& m = m_vm.messages[i];
        y += layout_rows(m, y);
        y += 1;  // 消息间距
        const std::size_t hit_start = m_hits.size();
        es.push_back(ftxui::hbox({
            ftxui::text("  "),
            ftxui::flex(build_message(m, width, m_anim_frame, &m_hits)),
        }));
        for (std::size_t k = hit_start; k < m_hits.size(); ++k)
            m_hits[k].msg_idx = static_cast<int>(i);
        // 消息间空一行，避免相邻消息（尤其用户块）紧贴；与 layout_rows 的 +1 对应
        es.push_back(ftxui::text(" "));
    }
    auto content = ftxui::vbox(std::move(es));

    int dimy = ftxui::Terminal::Size().dimy;
    // 减去：顶栏 1 + 输入区 kComposerHeight + 状态行 1 + 余量 2
    int avail = std::max(1, dimy - (1 + 1 + kComposerHeight + 2));
    int content_h = approx_height(width);
    int max_scroll = std::max(0, content_h - avail);

    if (m_follow) {
        // 跟随：钉在实际内容底部（focusPositionRelative 无估算误差）
        // 同时维护 m_scroll（估计底部），便于切到手动滚动时平滑衔接
        m_scroll = max_scroll;
        return content | ftxui::focusPositionRelative(0, 1.0f) | ftxui::frame;
    }

    // 手动：+avail/2 抵消 frame 的居中，使 m_scroll 即视口顶行（增大=向下=更新）
    m_scroll = std::max(0, std::min(m_scroll, max_scroll));
    if (m_scroll >= max_scroll) {
        // 已滚到底部：恢复自动跟随（新消息到来时继续钉在底部）
        m_follow = true;
        return content | ftxui::focusPositionRelative(0, 1.0f) | ftxui::frame;
    }
    return content
        | ftxui::focusPosition(0, m_scroll + avail / 2)
        | ftxui::frame;
}

Element App::build_ask_modal() const {
    if (!m_ask_active) return ftxui::emptyElement();
    return ftxui::border(ftxui::vbox({
        ftxui::text("❓ " + m_ask_question) | ftxui::bold,
        ftxui::separatorEmpty(),
        ftxui::hbox({ftxui::text("  输入答案（Enter 确认 · Esc 取消）") | ftxui::color(theme::T::Text)}),
        ftxui::hbox({
            ftxui::text("  "),
            ftxui::flex(m_ask_input->Render()),
        }),
    })) | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 60);
}

// ---------------------------------------------------------------------------
// 主循环
// ---------------------------------------------------------------------------

void App::run() {
#if defined(_WIN32)
    // stdout 为管道（如 opencode 终端）时 FTXUI 尺寸检测回退 80x24，
    // 用 CONOUT$ 兜底校准，避免渲染宽度不足导致右侧露白。
    int w = 0, h = 0;
    if (detect_console_size(w, h)) {
        ftxui::Terminal::SetFallbackSize({w, h});
    }
#endif
    // 状态行 / 侧栏：直接构建 Element（装饰性，非聚焦组件）
    auto build_sidebar_elem = [this] {
        return build_sidebar(m_vm.sidebar)
            | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, 30)
            | ftxui::yflex
            | ftxui::bgcolor(theme::T::Panel);
    };
    auto build_status_elem = [this] {
        return build_status_line(m_vm.sidebar.model, m_vm.sidebar.permission,
                                 m_vm.busy);
    };

    // 命令面板：ftxtui 独立注册表（内置中英文）+ agent 注册表命令注入
    CommandRegistry cmd_reg = CommandRegistry::builtins();
    if (m_deps.command_registry) {
        for (const auto& c : m_deps.command_registry->get_user_invocable_commands()) {
            cmd_reg.add(PaletteCommand{
                .command = "/" + c->name(),
                .title = c->name(),
                .keywords = c->description(),
            });
        }
    }
    std::vector<PaletteCommand> palette_cmds = cmd_reg.all();

    ComposerOptions comp_opt;
    comp_opt.buffer = &m_input_buffer;
    comp_opt.on_submit = [this](const std::string& t) { send_input(t); };
    comp_opt.on_perm_toggle = [this] {
        // 真实模式：session 侧切换并回读；mock 模式：本地循环 "" → plan → bypass
        if (m_deps.session) {
            m_deps.session->toggle_permission_mode();
            m_vm.sidebar.permission = mode_label(m_deps.session->permission_mode());
        } else {
            static const char* kCycle[] = {"", "plan", "bypass"};
            m_mock_perm_cycle = (m_mock_perm_cycle + 1) % 3;
            m_vm.sidebar.permission = kCycle[m_mock_perm_cycle];
        }
        m_vm.apply(ActionPermissions{.label = m_vm.sidebar.permission});
    };
    comp_opt.on_command_palette = [&] {
        m_palette_open = true;
        if (m_palette_comp) m_palette_comp->TakeFocus();
    };
    comp_opt.on_toggle_thinking = [this] {
        if (!m_vm.messages.empty()) {
            auto& m = m_vm.messages.back();
            if (m.reasoned) {
                m.reasoning_expanded = !m.reasoning_expanded;
                m_screen.RequestAnimationFrame();
            }
        }
    };

    m_composer = make_composer(comp_opt);

    // AskUser 模态输入（始终存在于组件树，按 m_ask_active 显隐）
    m_ask_input = ftxui::Input(&m_ask_buffer, "输入答案…");

    m_palette_comp = make_command_palette(
        palette_cmds,
        [this, palette_cmds](int idx) {
            if (idx < 0 || idx >= static_cast<int>(palette_cmds.size())) {
                m_palette_open = false;
                return;
            }
            const std::string& cmd = palette_cmds[static_cast<size_t>(idx)].command;
            if (cmd == "/model") {
                open_model_selector();
            } else {
                send_input(cmd);
            }
            m_palette_open = false;
        },
        m_palette_open,
        [this] { if (m_composer) m_composer->TakeFocus(); });

    m_model_comp = make_model_selector(
        m_model_items,
        [this](int idx) { apply_model(idx); m_model_open = false; },
        m_model_open);

    // 可聚焦组件栈：composer、AskUser 输入、命令面板、模型选择
    auto container = ftxui::Container::Vertical({
        m_composer,
        m_ask_input,
        m_palette_comp,
        m_model_comp,
    });

    auto layout = ftxui::Renderer(container, [&] {
        ++m_anim_frame;  // 推进思考动画帧
#if defined(_WIN32)
        // 无 tty（stdout 为管道）时 FTXUI 无法感知终端 resize：
        // 每帧用 CONOUT$ 校准 fallback 尺寸，检测变化后当帧生效
        // （App::Draw 在 component->Render() 之后才取 Terminal::Size()）。
        {
            int w = 0, h = 0;
            if (detect_console_size(w, h)) {
                auto cur = ftxui::Terminal::Size();
                if (cur.dimx != w || cur.dimy != h) {
                    ftxui::Terminal::SetFallbackSize({w, h});
                }
            }
        }
#endif
        int width = ftxui::Terminal::Size().dimx;

        Element sidebar_elem = build_sidebar_elem();
        Element left_col = ftxui::flex(build_transcript(width))
            | ftxui::bgcolor(theme::T::Surface);

        // 输入区：固定较高高度 + 上/下/左内边距（灰底由底部面板统一提供）
        Element input_body = ftxui::vbox({
            ftxui::text(" "),                       // 顶部内边距（占一行）
            ftxui::hbox({                           // 左侧内边距
                ftxui::text("  "),
                ftxui::flex(m_composer->Render()),
            }),
            ftxui::text(" "),                       // 底部内边距（占一行）
        }) | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, kComposerHeight);

        // 状态行内嵌在输入区下方，与输入区共用灰底与左侧边框（左缩进 2 格）
        Element status_elem = ftxui::hbox({
            ftxui::text("  "),
            ftxui::flex(build_status_elem()),
        });

        // 底部面板：输入区 + 状态行（看起来内嵌一体）
        Element bottom_panel = ftxui::vbox({
            input_body,
            status_elem,
        }) | ftxui::bgcolor(theme::T::Panel);

        // 左侧高亮边框线贯穿整个底部面板（输入区 + 状态行）
        constexpr int kStatusHeight = 1;
        constexpr int kPanelHeight = kComposerHeight + kStatusHeight;
        Elements border_lines;
        for (int i = 0; i < kPanelHeight; ++i)
            border_lines.push_back(ftxui::text("│"));
        Element border = ftxui::vbox(std::move(border_lines))
            | ftxui::color(theme::T::Accent);

        Element composer_zone = ftxui::hbox({
            border,
            bottom_panel | ftxui::flex,
        });

        // 左列：标题 + 转录 + 输入区（含内嵌状态行）
        Element content_col = ftxui::vbox({
            ftxui::text("WorkX · FTXUI 实验") | ftxui::color(theme::T::Text),
            left_col | ftxui::yflex,
            build_ask_modal(),
            composer_zone,
        });

        // 侧边栏占满右侧整列（顶到底），输入框右边即为侧边栏
        bool show_sidebar = width >= kSidebarCollapseWidth;
        Element body = ftxui::hbox({
            content_col | ftxui::flex,
            show_sidebar ? ftxui::text(" ") : ftxui::emptyElement(),
            show_sidebar ? sidebar_elem : ftxui::emptyElement(),
        });

        // 面板以居中叠加方式呈现（不压缩内容区）
        // 命令面板：悬浮于主会话之上、不整屏清空背景（四周可见会话内容）
        Elements layers;
        layers.push_back(body);
        if (m_palette_open)
            layers.push_back(ftxui::center(m_palette_comp->Render()));
        if (m_model_open)
            layers.push_back(ftxui::center(m_model_comp->Render()) | ftxui::clear_under);
        // 整个背景使用黑色
        return ftxui::dbox(std::move(layers)) | ftxui::bgcolor(theme::T::Canvas);
    });

    auto root = layout | ftxui::CatchEvent([&](Event e) {
        if (e == Event::Custom) {
            drain();
            // 思考动画期间持续推进重绘（动画线程定时 Post Custom）
            if (m_busy) m_screen.RequestAnimationFrame();
            return true;
        }
        if (m_ask_active) {
            if (e == Event::Return) { close_ask(true); m_screen.RequestAnimationFrame(); return true; }
            if (e == Event::Escape) { close_ask(false); m_screen.RequestAnimationFrame(); return true; }
        }
        if (e.is_mouse()) {
            if (e.mouse().button == ftxui::Mouse::WheelUp) {
                m_follow = false;
                m_scroll = std::max(0, m_scroll - 3);
                m_screen.RequestAnimationFrame();
                return true;
            }
            if (e.mouse().button == ftxui::Mouse::WheelDown) {
                m_scroll += 3;
                m_screen.RequestAnimationFrame();
                return true;
            }
            // 点击折叠卡片：展开/收起思考或工具内容（直接用渲染 box 命中）
            if (e.mouse().button == ftxui::Mouse::Left &&
                e.mouse().motion == ftxui::Mouse::Pressed) {
                for (const auto& hit : m_hits) {
                    if (e.mouse().x >= hit.box.x_min && e.mouse().x <= hit.box.x_max &&
                        e.mouse().y >= hit.box.y_min && e.mouse().y <= hit.box.y_max) {
                        auto& msg = m_vm.messages[static_cast<std::size_t>(hit.msg_idx)];
                        if (hit.tool_idx < 0) {
                            msg.reasoning_expanded = !msg.reasoning_expanded;
                        } else {
                            auto& t = msg.tool_calls[static_cast<std::size_t>(hit.tool_idx)];
                            t.expanded = !t.expanded;
                        }
                        m_screen.RequestAnimationFrame();
                        return true;
                    }
                }
            }
        }
        // 鼠标滚轮在管道终端（如 opencode）可能不转发事件，提供键盘替代：
        // Ctrl+↑/↓ 逐行滚动，PageUp/PageDown 整页滚动
        if (e == Event::ArrowUpCtrl) {
            m_follow = false;
            m_scroll = std::max(0, m_scroll - 3);
            m_screen.RequestAnimationFrame();
            return true;
        }
        if (e == Event::ArrowDownCtrl) {
            m_scroll += 3;
            m_screen.RequestAnimationFrame();
            return true;
        }
        // 滚一页（转录区可视高度），PageUp 翻回上页、PageDown 翻回下页
        const int scroll_page =
            std::max(1, ftxui::Terminal::Size().dimy - (1 + 1 + kComposerHeight + 2));
        if (e == Event::PageDown) {
            m_scroll += scroll_page;
            m_screen.RequestAnimationFrame();
            return true;
        }
        if (e == Event::PageUp) {
            m_follow = false;
            m_scroll = std::max(0, m_scroll - scroll_page);
            m_screen.RequestAnimationFrame();
            return true;
        }
        return false;
    });

    // 重绘驱动线程：定时触发重绘，驱动思考动画并校准终端尺寸
    // （无 tty 时 FTXUI 感知不到 resize，需要持续重绘才能每帧校准）
    m_anim_run = true;
    m_anim_thread = std::thread([this] {
        while (m_anim_run) {
            m_screen.PostEvent(Event::Custom);
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
        }
    });

    try {
        m_screen.Loop(root);
    } catch (const std::exception& ex) {
        log_run(std::string("loop exception: ") + ex.what());
        throw;
    }
}

// ---------------------------------------------------------------------------
// helper：权限模式标签
// ---------------------------------------------------------------------------

std::string App::mode_label(agent::tool::PermissionMode m) {
    switch (m) {
        case agent::tool::PermissionMode::Plan: return "plan";
        case agent::tool::PermissionMode::BypassPermissions: return "bypass";
        default: return "";
    }
}

}  // namespace ftxtui