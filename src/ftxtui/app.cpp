#include "app.h"
#include "crash_reporter.h"
#include "liblogger/logger.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#undef RGB
#endif

#include <ftxui/component/event.hpp>
#include <ftxui/component/animation.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/dom/direction.hpp>
#include <ftxui/screen/terminal.hpp>

#include "agent/api/i_backend_admin.h"
#include "agent/api/chat_types.h"
#include "agent/command/inclaude/registry.h"
#include "agent/config/app_config.h"
#include "agent/core/chat_session.h"
#include "agent/input/processor.h"
#include "agent/model/provider_config.h"
#include "agent/session/session_store.h"
#include "agent/skill/inclaude/skill_loader.h"
#include "command/builtins.h"
#include "core/events/stream_events.h"
#include "theme/icons.h"
#include "theme/strings.h"
#include "theme/theme.h"
#include "clipboard.h"
#include "render/markdown_to_elements.h"
#include "render/transcript_layout.h"
#include "widgets/sidebar.h"
#include "widgets/status_line.h"
#include "widgets/composer.h"
#include "widgets/suggest_panel.h"
#include "widgets/search_palette.h"
#include "core/utils/file_index.h"

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
/// @brief 侧栏固定宽度（列）；输出区可用宽度 = 终端宽 − 侧栏 − 1 分隔空格
constexpr int kSidebarWidth = 30;

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

/// @brief 统计 UTF-8 文本的字符数（去除尾随空白/换行，供「已复制 N 字符」提示）
std::size_t utf8_char_count(std::string_view s) {
    std::size_t end = s.size();
    while (end > 0) {
        const unsigned char c = static_cast<unsigned char>(s[end - 1]);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { --end; continue; }
        break;
    }
    s = s.substr(0, end);
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if ((c & 0xC0) != 0x80) ++n;  // 非续字节 = 新码点
    }
    return n;
}
}  // namespace

App::App(AppDeps deps)
    : m_deps(std::move(deps)),
      m_bridge(*m_deps.event_bus, m_queue),
      m_screen(ftxui::ScreenInteractive::Fullscreen()) {
    m_vm.sidebar.model = m_deps.model_name;
    m_vm.sidebar.project = m_deps.project;
    m_vm.sidebar.agent = m_deps.agent_name;

    // B2 统一命令：单一定义 — 内置命令注册进 agent 注册表，副作用回调到 App
    if (!m_deps.command_registry) {
        m_deps.command_registry = std::make_shared<agent::command::CommandRegistry>();
    }
    register_ftx_builtins(*m_deps.command_registry, FtuiCommandCallbacks{
        .on_exit = [this] {
            m_vm.apply(ActionShutdown{});
            if (m_vm.pending_exit) m_screen.Exit();
        },
        .on_model_select = [this] { open_model_selector(); },
        .on_provider_select = [this] { open_provider_palette(); },
        .on_resume = [this](const std::string& args) { cmd_resume(args); },
        .on_rename = [this](const std::string& args) { cmd_rename(args); },
        .on_clear = [this] {
            if (m_deps.session) m_deps.session->clear_history();
            m_vm.messages.clear();
            invalidate_msg_cache();
            m_scroll = 0;
        },
        .on_test_askuser = [this] { cmd_test_askuser(); },
    });

    // B2 统一命令：App 持有唯一命令处理器（消费 agent 注册表，含内置命令）
    m_command_processor = std::make_unique<agent::input::InputProcessor>(
        m_deps.command_registry, std::make_shared<agent::input::LocalFileLoader>());

    // 加载磁盘 skills（.claude/skills）并注册为命令 → "/" 提示面板与 Skill 工具共用
    // （对齐 src/app/main.cpp；终端 author 在 ftxtui 路径保留 skill 为斜杠命令可执行）
    {
        namespace fs = std::filesystem;
        auto base_dirs = agent::skill::find_skill_dirs_up_to_home(fs::current_path().string());
        for (const auto& dir : agent::skill::find_user_skill_dirs()) base_dirs.push_back(dir);
        const auto skills = agent::skill::load_skills_from_dirs(base_dirs);
        for (const auto& s : skills) m_deps.command_registry->register_command(s);
        // conditional skills：会话持有命令注册表（激活匹配 SkillTool 用）
        if (m_deps.session) m_deps.session->set_command_registry(m_deps.command_registry);
    }

    m_bridge.set_wake_callback([this] { m_screen.PostEvent(Event::Custom); });
    m_bridge.start();

    // 拖拽选中：FTXUI 原生高亮选区（左键按下/移动/释放由 App::HandleSelection
    // 消费），选区文本变化时缓存，供鼠标释放时写入系统剪贴板。
    m_screen.SelectionChange([this] { on_selection_changed(); });

    // 关闭 FTXUI 对 Ctrl+C 的强制处理：默认 force_handle_ctrl_c_=true 会让
    // 每次 Ctrl+C 都 RecordSignal(SIGINT) → Exit()，导致单次按下即退出。
    // 关闭后仅当组件树未消费该事件时才退出；本应用在 CatchEvent 内自行实现
    // 「1 秒内连按两次 Ctrl+C 才退出」的语义。
    m_screen.ForceHandleCtrlC(false);
}

App::~App() {
    m_anim_run = false;
    m_anim_cv.notify_all();
    if (m_anim_thread.joinable()) m_anim_thread.join();
    m_stream_run = false;
    if (m_stream_thread.joinable()) m_stream_thread.join();
    // 复制提示自清除线程不触碰任何成员（析构安全）；join 等待它结束时 m_screen 仍存活。
    if (m_copy_flash_thread.joinable()) m_copy_flash_thread.join();
    m_bridge.stop();
}

// ---------------------------------------------------------------------------
// 每帧事件处理
// ---------------------------------------------------------------------------

void App::handle_ask_user(const ActionAskUser& a) {
    m_ask_promise = a.result_promise;
    m_ask_cancel = a.cancel_flag;

    // B3：解析全部问题（多问题 + 选项 + 自定义输入开关）
    // 兼容两种 questions 契约：AskUserTool 发布 {questions:[...]} 对象，
    // permission_ask 发布直接数组。
    m_ask_questions.clear();
    m_ask_answers.clear();
    const nlohmann::json* qs = nullptr;
    if (a.questions.is_array()) {
        qs = &a.questions;
    } else if (a.questions.is_object() && a.questions.contains("questions") &&
               a.questions["questions"].is_array()) {
        qs = &a.questions["questions"];
    }
    if (qs) {
        for (const auto& q : *qs) {
            if (!q.is_object()) continue;
            AskQuestion aq;
            aq.question = q.value("question", "");
            aq.header = q.value("header", "");
            aq.multi = q.value("multiSelect", false);
            aq.allow_custom_input = q.value("allow_custom_input", true);
            if (q.contains("options") && q["options"].is_array()) {
                for (const auto& o : q["options"]) {
                    if (o.is_object()) aq.options.push_back(o.value("label", ""));
                    else if (o.is_string()) aq.options.push_back(o.get<std::string>());
                }
            }
            if (aq.question.empty() && !aq.header.empty()) aq.question = aq.header;
            if (!aq.question.empty() && !aq.options.empty()) m_ask_questions.push_back(std::move(aq));
        }
    }

    // 无有效问题：不激活模态，直接以取消结果回填，避免空向量越界崩溃
    if (m_ask_questions.empty()) {
        close_ask(false);
        return;
    }

    m_ask_active = true;
    m_ask_qindex = 0;
    m_ask_sel = 0;
    m_ask_custom = false;
    m_ask_checked.assign(m_ask_questions[0].options.size(), false);
    m_ask_buffer.clear();
    m_ask_deadline = a.timeout_ms > 0
        ? std::chrono::steady_clock::now() + std::chrono::milliseconds(a.timeout_ms)
        : std::chrono::steady_clock::time_point::max();
    if (m_ask_input) m_ask_input->TakeFocus();
}

/// @brief 记录当前问题答案，前进到下一题；最后一题则提交
/// @return true=已全部答完并提交，false=还有下一题
bool App::advance_ask() {
    if (m_ask_qindex >= m_ask_questions.size()) {
        close_ask(true);
        return true;
    }
    const AskQuestion& q = m_ask_questions[m_ask_qindex];
    if (m_ask_custom) {
        // 自定义输入：取文本（去首尾空白）
        std::string ans = m_ask_buffer;
        size_t b = ans.find_first_not_of(" \t");
        size_t e = ans.find_last_not_of(" \t");
        if (b == std::string::npos) ans.clear();
        else ans = ans.substr(b, e - b + 1);
        if (ans.empty()) {
            // 空自定义输入视为取消提交（保留当前题继续）
            return false;
        }
        m_ask_answers.emplace_back(q.question, std::move(ans));
    } else if (q.multi) {
        // 多选：已勾选项用逗号连接
        std::string ans;
        for (size_t i = 0; i < m_ask_checked.size(); ++i) {
            if (m_ask_checked[i] && i < q.options.size()) {
                if (!ans.empty()) ans += ", ";
                ans += q.options[i];
            }
        }
        if (ans.empty()) return false;  // 未选任何项，不前进
        m_ask_answers.emplace_back(q.question, std::move(ans));
    } else {
        // 单选：选中项 label；选到自定义输入选项则进入输入模式
        if (m_ask_sel >= 0 && static_cast<size_t>(m_ask_sel) < q.options.size()) {
            m_ask_answers.emplace_back(q.question, q.options[static_cast<size_t>(m_ask_sel)]);
        } else {
            return false;
        }
    }
    // 下一题
    ++m_ask_qindex;
    if (m_ask_qindex >= m_ask_questions.size()) {
        close_ask(true);
        return true;
    }
    m_ask_sel = 0;
    m_ask_custom = false;
    m_ask_buffer.clear();
    m_ask_checked.assign(m_ask_questions[m_ask_qindex].options.size(), false);
    return false;
}

void App::close_ask(bool submitted) {
    const bool echo = m_ask_test_echo;
    m_ask_test_echo = false;
    if (m_ask_promise) {
        agent::AskUserResult r;
        r.submitted = submitted;
        if (submitted) {
            r.answers = m_ask_answers;
        }
        m_ask_promise->set_value(std::move(r));
        m_ask_promise.reset();
    }
    // /Test:askuser 调试回显：把返回结果作为 assistant 消息展示，便于核对答案
    if (echo) {
        std::string text;
        if (submitted) {
            text = std::string(str::kTestAskUserPrefix);
            for (const auto& [q, a] : m_ask_answers) {
                text += "• " + q + " → " + a + "\n";
            }
        } else {
            text = std::string(str::kTestAskUserCancelled);
        }
        if (!text.empty()) {
            m_vm.apply(ActionAppendMessage{.role = "assistant", .text = std::move(text)});
            m_screen.PostEvent(Event::Custom);
        }
    }
    m_ask_active = false;
    m_ask_buffer.clear();
    m_ask_cancel.reset();
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
                rebuild_model_entries();  // /model 面板条目跟随刷新（active 标记）
                changed = true;
                continue;
            }
            if (auto* sessions = std::get_if<ActionSessionsLoaded>(&a)) {
                m_session_metas = sessions->sessions;
                m_sessions_loading = false;
                // /resume 面板打开中则同步条目（会话可能刚加载完）
                if (m_resume_open) {
                    m_session_entries.clear();
                    for (size_t i = 0; i < m_session_metas.size(); ++i) {
                        const auto& s = m_session_metas[i];
                        m_session_entries.push_back(SearchEntry{
                            .category = SearchCategory::Session,
                            .title = s.title,
                            .subtitle = s.project_name,
                            .keywords = std::to_string(s.message_count)
                                        + std::string(str::kMsgCountKeyword),
                            .payload = static_cast<int>(i),
                        });
                    }
                }
                // 聚合搜索面板（Ctrl+P）打开中：重新装配条目，让会话记录即时出现
                if (m_palette_open) {
                    m_search_entries = assemble_search_entries();
                }
                changed = true;  // 聚合搜索面板下一帧自动重新过滤（数据已更新）
                continue;
            }
            if (auto* switched = std::get_if<ActionProviderSwitched>(&a)) {
                handle_provider_switched(std::move(switched->provider),
                                         switched->model_name, switched->entry);
                changed = true;
                continue;
            }
            if (auto* failed = std::get_if<ActionProviderSwitchFailed>(&a)) {
                handle_provider_switch_failed(failed->provider_name);
                changed = true;
                continue;
            }
            changed |= m_vm.apply(a);
        } catch (const std::exception& ex) {
            log_run(std::string("drain: action exception: ") + ex.what());
        }
    }
    // 同步忙标志（供动画线程读取，避免跨线程读 ViewModel）；变化时唤醒动画线程
    const bool new_busy = m_vm.busy;
    if (m_busy.exchange(new_busy) != new_busy) {
        m_anim_cv.notify_all();
    }
    // 消息数镜像（冒烟 driver 只读此 atomic，避免与 UI 线程并发读 ViewModel）
    m_msg_count.store(m_vm.messages.size());
    // B3：AskUser 超时 / cancel_flag 检查（工作线程超时后置位取消标志）
    if (m_ask_active) {
        bool cancelled = m_ask_cancel && m_ask_cancel->load();
        bool expired = m_ask_deadline != std::chrono::steady_clock::time_point::max()
                       && std::chrono::steady_clock::now() >= m_ask_deadline;
        if (cancelled || expired) {
            close_ask(false);
            changed = true;
        }
    }
    if (changed) m_screen.RequestAnimationFrame();
    if (m_vm.pending_exit) m_screen.Exit();
}

void App::log_run(std::string_view msg) {
    std::lock_guard<std::mutex> lock(m_log_mutex);
    // B4：日志路径平台无关，统一写 ~/.workx/logs/codex_run.log（复用 agent 配置约定）
    namespace fs = std::filesystem;
    fs::path log_path = agent::default_log_path().parent_path() / "codex_run.log";
    std::error_code ec;
    if (auto parent = log_path.parent_path(); !parent.empty()) {
        fs::create_directories(parent, ec);
    }
    FILE* f = fopen(log_path.string().c_str(), "a");
    if (!f) return;
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
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
            .text = std::string(str::kNoBackendModels)});
        return;
    }
    // 面板互斥：同一时刻只开一个悬浮面板
    m_palette_open = false;
    m_resume_open = false;
    m_provider_open = false;
    m_model_items.clear();
    rebuild_model_entries();  // 空列表（加载中）
    m_model_open = true;
    if (m_model_comp) m_model_comp->TakeFocus();
    // 后台拉取模型列表，完成后入队 action 刷新（避免阻塞 UI）
    std::thread([this]() {
        auto result = m_deps.backend_admin->list_models();
        std::vector<std::string> names;
        if (result.is_ok()) {
            for (const auto& m : result.value())
                names.push_back(m.name);
        } else {
            names.push_back(std::string(str::kModelListFailed));
        }
        m_queue.push(ActionModelsLoaded{.models = std::move(names)});
        m_screen.PostEvent(Event::Custom);
    }).detach();
}

void App::rebuild_model_entries() {
    m_model_entries.clear();
    const std::string& cur = m_vm.sidebar.model;  // 当前模型
    for (size_t i = 0; i < m_model_items.size(); ++i) {
        m_model_entries.push_back(SearchEntry{
            .category = SearchCategory::Model,
            .title = m_model_items[i],
            .payload = static_cast<int>(i),
            .active = (m_model_items[i] == cur),
        });
    }
}

void App::apply_model(int index) {
    if (index < 0 || index >= static_cast<int>(m_model_items.size())) return;
    const std::string& name = m_model_items[static_cast<size_t>(index)];
    if (name.rfind(std::string(str::kModelListFailed), 0) == 0) return;  // 占位错误行，忽略
    if (m_deps.backend_admin) m_deps.backend_admin->set_model_name(name);
    m_vm.sidebar.model = name;
    if (m_deps.on_model_changed) m_deps.on_model_changed();
}

void App::cmd_resume(const std::string& args) {
    if (!m_deps.session || m_deps.session_dir.empty()) {
        m_vm.apply(ActionAppendMessage{.role = "assistant",
            .text = std::string(str::kNoSessionBackend)});
        return;
    }

    if (args.empty()) {
        // 无参：打开会话选择面板（复用搜索面板组件）
        open_resume_palette();
        return;
    }

    // 带编号：恢复对应会话
    auto sessions = agent::session::SessionStore::list_sessions(m_deps.session_dir);
    if (sessions.empty()) {
        m_vm.apply(ActionAppendMessage{.role = "assistant",
            .text = std::string(str::kNoHistorySessions)});
        return;
    }
    int idx = 0;
    try { idx = std::stoi(args); } catch (...) { idx = 0; }
    if (idx < 1 || idx > static_cast<int>(sessions.size())) {
        m_vm.apply(ActionAppendMessage{.role = "assistant",
            .text = std::string(str::kResumeBadIndex)});
        return;
    }
    const auto& s = sessions[static_cast<size_t>(idx - 1)];
    const std::string title = s.title.empty() ? s.session_id : s.title;
    resume_session(s.file_path, title);
}

void App::resume_session(const std::string& file_path, const std::string& title) {
    if (!m_deps.session) return;
    if (!m_deps.session->switch_session(file_path)) {
        m_vm.apply(ActionAppendMessage{.role = "assistant",
            .text = std::string(str::kResumeFailedPrefix) + file_path
                    + std::string(str::kCloseParenNl)});
        return;
    }

    // 载入历史消息（含思考/工具卡片；tool 结果按 call_id 回填）
    m_vm.messages.clear();
    invalidate_msg_cache();
    const auto history = m_deps.session->get_messages();
    for (const auto& cm : history) {
        if (cm.role == agent::ChatMessage::Role::User) {
            m_vm.apply(ActionAppendMessage{.role = "user", .text = cm.content});
        } else if (cm.role == agent::ChatMessage::Role::Assistant) {
            // 构造含卡片的 assistant 节点：正文 + 思考卡片 + 工具卡片
            MessageNode n;
            n.role = MsgRole::Assistant;
            n.text = cm.content;
            n.sealed = true;
            if (!cm.reasoning_content.empty()) {
                n.reasoned = true;
                n.reasoning = cm.reasoning_content;
                n.reasoning_expanded = m_vm.card_defaults.reasoning_expanded;
                n.reasoning_ms = cm.reasoning_ms;
            }
            for (const auto& tu : cm.tool_uses) {
                ToolCallNode t;
                t.tool_name = tu.name;
                t.call_id = tu.id;
                t.arguments = tu.input.dump();
                n.tool_calls.push_back(std::move(t));
            }
            m_vm.messages.push_back(std::move(n));
        } else if (cm.role == agent::ChatMessage::Role::Tool) {
            // 工具结果：按 call_id 回填对应卡片（出错默认展开，对齐实时路径）
            for (auto it = m_vm.messages.rbegin(); it != m_vm.messages.rend(); ++it) {
                if (auto* t = it->find_tool(cm.tool_call_id)) {
                    t->result = cm.content;
                    t->done = true;
                    t->running = false;
                    t->is_error = cm.is_error;
                    t->expanded = cm.is_error;
                    break;
                }
            }
        }
    }
    m_scroll = 0;
    m_follow = true;
    m_vm.sidebar.title = title;
    // B3：会话切换后同步真实 session_id（switch_session 内部已更新）
    m_vm.sidebar.agent = m_deps.session->session_id();
    m_vm.apply(ActionAppendMessage{.role = "assistant",
        .text = std::string(str::kResumedPrefix) + title + std::string(str::kMdBoldEnd)});
}

void App::cmd_rename(const std::string& args) {
    if (args.empty()) {
        m_vm.apply(ActionAppendMessage{.role = "assistant",
            .text = std::string(str::kRenameUsage)});
        return;
    }
    auto store = m_deps.session ? m_deps.session->session_store() : nullptr;
    if (store) store->append_title(args);
    m_vm.sidebar.title = args;
    m_vm.apply(ActionAppendMessage{.role = "assistant",
        .text = std::string(str::kRenamedPrefix) + args + std::string(str::kMdBoldEnd)});
}

/// @brief /Test:askuser：直接用示例 questions 弹出 AskUser 提问弹窗（调试 TUI 渲染/交互）
/// @details 不经 EventBus/AskUserTool（纯 UI 测试），构建 ActionAskUser 直接进模态；
///          关闭时 close_ask 依据 m_ask_test_echo 把答案回显为 assistant 消息。
void App::cmd_test_askuser() {
    static const std::string input_str = R"JSON({
        "questions": [
            {
                "question": "选择一种认证方式？",
                "header": "认证",
                "multiSelect": false,
                "allow_custom_input": true,
                "options": [
                    {"label": "OAuth 2.0", "description": "委托授权（推荐）"},
                    {"label": "API Key", "description": "长期密钥"},
                    {"label": "Basic Auth", "description": "用户名密码"}
                ]
            },
            {
                "question": "要启用哪些功能？",
                "header": "功能",
                "multiSelect": true,
                "allow_custom_input": true,
                "options": [
                    {"label": "代码检索", "description": "跨文件搜索"},
                    {"label": "语法高亮", "description": "树形解析"},
                    {"label": "文件索引", "description": "异步建索引"},
                    {"label": "实时协作", "description": "多端同步"}
                ]
            }
        ]
    })JSON";
    m_ask_test_echo = true;
    auto promise = std::make_shared<std::promise<agent::AskUserResult>>();
    handle_ask_user(ActionAskUser{
        .questions = nlohmann::json::parse(input_str),
        .timeout_ms = 0,          // 不限时，便于慢慢手动测试
        .result_promise = std::move(promise),
        .cancel_flag = nullptr,
    });
    m_screen.PostEvent(Event::Custom);  // 唤醒事件循环重绘模态
}

// ---------------------------------------------------------------------------
// 统一悬浮面板：/resume 会话 · /provider 供应商（复用 search_palette 组件）
// ---------------------------------------------------------------------------

void App::open_resume_palette() {
    // 面板互斥：同一时刻只开一个悬浮面板
    m_palette_open = false;
    m_model_open = false;
    m_provider_open = false;
    ensure_sessions_loaded();
    m_session_entries.clear();
    for (size_t i = 0; i < m_session_metas.size(); ++i) {
        const auto& s = m_session_metas[i];
        m_session_entries.push_back(SearchEntry{
            .category = SearchCategory::Session,
            .title = s.title,
            .subtitle = s.file_path,
            .keywords = std::to_string(s.message_count) + std::string(str::kMsgCountKeyword),
            .payload = static_cast<int>(i),
        });
    }
    m_resume_open = true;
    if (m_resume_comp) m_resume_comp->TakeFocus();
}

void App::open_provider_palette() {
    if (!m_deps.config_manager) {
        m_vm.apply(ActionAppendMessage{.role = "assistant",
            .text = std::string(str::kNoProviderConfig)});
        return;
    }
    // 面板互斥：同一时刻只开一个悬浮面板
    m_palette_open = false;
    m_resume_open = false;
    m_model_open = false;
    m_providers = agent::load_provider_configs(*m_deps.config_manager);
    m_current_provider =
        m_deps.config_manager->get_or<std::string>(agent::keys::PROVIDER, "");
    if (m_providers.empty()) {
        m_vm.apply(ActionAppendMessage{.role = "assistant",
            .text = std::string(str::kNoProvidersConfigured)});
        return;
    }
    assemble_provider_entries();
    m_provider_open = true;
    if (m_provider_comp) m_provider_comp->TakeFocus();
}

void App::assemble_provider_entries() {
    m_provider_entries.clear();
    for (size_t i = 0; i < m_providers.size(); ++i) {
        const auto& p = m_providers[i];
        m_provider_entries.push_back(SearchEntry{
            .category = SearchCategory::Provider,
            .title = p.name,
            .subtitle = p.base_url,
            .payload = static_cast<int>(i),
            .active = (p.id == m_current_provider),
        });
    }
}

void App::switch_provider(int index) {
    if (index < 0 || index >= static_cast<int>(m_providers.size())) return;
    const agent::ProviderConfigEntry entry = m_providers[static_cast<size_t>(index)];
    if (!m_deps.create_provider) {
        m_vm.apply(ActionAppendMessage{.role = "assistant",
            .text = std::string(str::kProviderSwitchFailedPrefix) + entry.name
                    + std::string(str::kCloseParenNl)});
        return;
    }
    // 正在生成中拒绝热切换（ReAct 循环持有 provider；run_completion 期间换后端竞态）
    if (m_deps.session && m_deps.session->is_generating()) {
        m_vm.apply(ActionAppendMessage{.role = "assistant",
            .text = std::string(str::kProviderBusy) + entry.name
                    + std::string(str::kCloseParenNl)});
        return;
    }
    // 后台创建新后端（不阻塞 UI）；完成后经事件队列回 UI 线程执行热切换
    std::thread([this, entry] {
        auto result = m_deps.create_provider(entry.id);
        if (!result.provider) {
            m_queue.push(ActionProviderSwitchFailed{.provider_name = entry.name});
            m_screen.PostEvent(Event::Custom);
            return;
        }
        m_queue.push(ActionProviderSwitched{
            .provider = std::move(result.provider),
            .model_name = result.model_name,
            .entry = entry,
        });
        m_screen.PostEvent(Event::Custom);
    }).detach();
}

void App::handle_provider_switched(std::unique_ptr<agent::ICompletionProvider> provider,
                                   const std::string& model_name,
                                   const agent::ProviderConfigEntry& entry) {
    if (!m_deps.session) return;
    // 保留当前对话继续（import_messages 重置上下文压缩基线，不丢历史）
    auto messages = m_deps.session->get_messages();
    if (!m_deps.session->set_provider(std::move(provider))) {
        // 竞态（处理时已开始生成）：拒绝本次切换，新后端随 unique_ptr 析构
        m_vm.apply(ActionAppendMessage{.role = "assistant",
            .text = std::string(str::kProviderBusy) + entry.name
                    + std::string(str::kCloseParenNl)});
        return;
    }
    m_deps.session->import_messages(std::move(messages));
    // 刷新 admin 句柄（旧指针已随旧 provider 失效）
    if (auto* p = m_deps.session->completion_provider())
        m_deps.backend_admin = dynamic_cast<agent::IBackendAdmin*>(p);
    // 持久化切换（成功后写配置，避免失败残留）
    if (m_deps.config_manager)
        agent::apply_provider_switch(*m_deps.config_manager, entry);
    // 更新侧栏模型显示与模型列表 active 标记
    if (!model_name.empty()) {
        m_vm.sidebar.model = model_name;
        if (m_deps.on_model_changed) m_deps.on_model_changed();
    }
    rebuild_model_entries();
    m_vm.apply(ActionAppendMessage{.role = "assistant",
        .text = std::string(str::kProviderSwitchedPrefix) + entry.name
                + std::string(str::kMdBoldEnd)});
}

void App::handle_provider_switch_failed(const std::string& provider_name) {
    m_vm.apply(ActionAppendMessage{.role = "assistant",
        .text = std::string(str::kProviderSwitchFailedPrefix) + provider_name
                + std::string(str::kCloseParenNl)});
}

// ---------------------------------------------------------------------------
// 输入栏提示面板（/ 命令 · @ 文件）：状态机由 composer 事件驱动
// ---------------------------------------------------------------------------

void App::update_suggest() {
    std::string query;
    const SuggestMode mode = parse_suggest_query(m_input_buffer, query);
    m_suggest_mode = mode;
    m_suggest_entries.clear();
    m_suggest_files.clear();

    if (mode == SuggestMode::Command) {
        // 命令候选：注册表派生条目（与搜索面板共用 m_palette_cmds）
        std::vector<std::string> searchable;
        searchable.reserve(m_palette_cmds.size());
        for (const auto& c : m_palette_cmds)
            searchable.push_back(c.command + " " + c.title + " " + c.keywords);
        for (const size_t i : filter_commands(searchable, query)) {
            m_suggest_entries.push_back(SuggestEntry{
                .title = m_palette_cmds[i].command,
                .subtitle = m_palette_cmds[i].title,
                .payload = static_cast<int>(i),
            });
        }
    } else if (mode == SuggestMode::File) {
        auto& fi = agent::global_file_index();
        if (fi.is_ready()) {
            m_suggest_files = fi.search(query, 15);
            for (size_t i = 0; i < m_suggest_files.size(); ++i) {
                m_suggest_entries.push_back(SuggestEntry{
                    .title = m_suggest_files[i].name,
                    .subtitle = m_suggest_files[i].relative_path,
                    .payload = static_cast<int>(i),
                });
            }
        }
    }
    // 选中项：过滤后保留（clamp），首次取 0
    if (m_suggest_entries.empty()) {
        m_suggest_selected = -1;
    } else if (m_suggest_selected < 0 ||
               m_suggest_selected >= static_cast<int>(m_suggest_entries.size())) {
        m_suggest_selected = 0;
    }
}

void App::suggest_move(int delta) {
    if (m_suggest_entries.empty()) return;
    const int n = static_cast<int>(m_suggest_entries.size());
    m_suggest_selected = (m_suggest_selected + delta) % n;
    if (m_suggest_selected < 0) m_suggest_selected += n;
}

bool App::suggest_enter() {
    if (m_suggest_mode == SuggestMode::None || m_suggest_selected < 0) return false;
    const auto& e = m_suggest_entries[static_cast<size_t>(m_suggest_selected)];
    if (m_suggest_mode == SuggestMode::Command) {
        // 命令面板：自动补全命令名 + 一个空格，关闭面板（不执行、不发送消息）
        if (e.payload >= 0 && e.payload < static_cast<int>(m_palette_cmds.size()))
            m_input_buffer =
                m_palette_cmds[static_cast<size_t>(e.payload)].command + " ";
        else
            m_input_buffer.clear();
        m_composer_cursor = m_input_buffer.size();
    } else if (m_suggest_mode == SuggestMode::File) {
        // 文件面板：把 @query 替换为 @路径 + 一个空格（不发送消息，交回输入框）
        const auto at = m_input_buffer.rfind('@');
        if (at == std::string::npos ||
            e.payload < 0 || e.payload >= static_cast<int>(m_suggest_files.size())) {
            suggest_cancel();
            return false;
        }
        m_input_buffer = m_input_buffer.substr(0, at + 1)
                         + m_suggest_files[static_cast<size_t>(e.payload)].relative_path
                         + " ";
        m_composer_cursor = m_input_buffer.size();
    }
    suggest_cancel();
    return true;
}

void App::suggest_cancel() {
    m_suggest_mode = SuggestMode::None;
    m_suggest_entries.clear();
    m_suggest_files.clear();
    m_suggest_selected = -1;
}

// ---------------------------------------------------------------------------
// 全局聚合搜索面板（Ctrl+P）：会话 / 文件 / 功能 / 设置
// ---------------------------------------------------------------------------

namespace {

/// @brief 设置动作（搜索面板「设置」类 payload）
enum class SettingAction {
    PermCycle,      ///< 切换权限模式
    ModelSelector,  ///< 打开模型选择器
    ProviderSelector, ///< 打开供应商切换面板
    ToggleThinking, ///< 折叠 / 展开思考
    ToggleSidebar,  ///< 切换侧边栏位置（左 / 右）
    Clear,          ///< 清空会话
    Exit,           ///< 退出
};

/// @brief 追加一条设置条目
void push_setting(std::vector<SearchEntry>& out, SettingAction action,
                  std::string_view title, std::string_view desc,
                  std::string_view keywords) {
    out.push_back(SearchEntry{
        .category = SearchCategory::Setting,
        .title = std::string(title),
        .subtitle = std::string(desc),
        .keywords = std::string(keywords),
        .payload = static_cast<int>(action),
    });
}

}  // namespace

void App::ensure_sessions_loaded() {
    // 会话：缓存；未加载则后台加载（面板本轮跳过，加载完成自动刷新）
    if (!m_session_metas.empty() || m_sessions_loading || m_deps.session_dir.empty()) return;
    m_sessions_loading = true;
    const std::string dir = m_deps.session_dir;
    std::thread([this, dir] {
        auto sessions = agent::session::SessionStore::list_sessions(dir);
        std::vector<SessionLite> lite;
        lite.reserve(sessions.size());
        for (const auto& s : sessions) {
            lite.push_back(SessionLite{
                .title = s.title.empty() ? s.session_id : s.title,
                .file_path = s.file_path,
                .project_name = s.cwd.empty()
                                    ? std::string()
                                    : std::filesystem::path(s.cwd).filename().string(),
                .message_count = s.message_count,
            });
        }
        m_queue.push(ActionSessionsLoaded{.sessions = std::move(lite)});
        m_screen.PostEvent(Event::Custom);
    }).detach();
}

std::vector<SearchEntry> App::assemble_search_entries() {
    std::vector<SearchEntry> out;
    out.reserve(m_palette_cmds.size() + 32);

    // 功能（命令/skill）：注册表派生命令（含磁盘 skills）；"/" 前缀可单独筛选
    for (size_t i = 0; i < m_palette_cmds.size(); ++i) {
        const auto& c = m_palette_cmds[i];
        out.push_back(SearchEntry{
            .category = SearchCategory::Feature,
            .title = c.title.empty() ? c.command : c.title,
            .subtitle = c.command,
            .keywords = c.keywords,
            .payload = static_cast<int>(i),
        });
    }

    // 文件：最近修改（索引就绪后）
    auto& fi = agent::global_file_index();
    if (fi.is_ready()) {
        const auto files = fi.search("", 15);
        for (size_t i = 0; i < files.size(); ++i) {
            out.push_back(SearchEntry{
                .category = SearchCategory::File,
                .title = files[i].name,
                .subtitle = files[i].relative_path,
                .payload = static_cast<int>(i),
            });
        }
    }

    // 会话：缓存；未加载则后台加载（面板本轮跳过，加载完成自动刷新）
    ensure_sessions_loaded();
    for (size_t i = 0; i < m_session_metas.size(); ++i) {
        const auto& s = m_session_metas[i];
        out.push_back(SearchEntry{
            .category = SearchCategory::Session,
            .title = s.title,
            .subtitle = s.project_name,
            .keywords = std::to_string(s.message_count) + std::string(str::kMsgCountKeyword),
            .payload = static_cast<int>(i),
        });
    }

    // 设置：静态条目
    push_setting(out, SettingAction::PermCycle, str::kSettingPerm, str::kSettingPermDesc,
                 "permission plan bypass 权限");
    push_setting(out, SettingAction::ModelSelector, str::kSettingModel, str::kSettingModelDesc,
                 "model 模型");
    push_setting(out, SettingAction::ProviderSelector, str::kSettingProvider,
                 str::kSettingProviderDesc, "provider 供应商");
    push_setting(out, SettingAction::ToggleThinking, str::kSettingThinking,
                 str::kSettingThinkingDesc, "think reasoning 思考");
    push_setting(out, SettingAction::ToggleSidebar, str::kSettingSidebar,
                 str::kSettingSidebarDesc, "sidebar side 侧边栏 位置");
    push_setting(out, SettingAction::Clear, str::kSettingClear, str::kSettingClearDesc,
                 "clear 清空");
    push_setting(out, SettingAction::Exit, str::kSettingExit, str::kSettingExitDesc,
                 "exit quit 退出");

    return out;
}

void App::apply_search_entry(int index) {
    if (index < 0 || index >= static_cast<int>(m_search_entries.size())) return;
    const SearchEntry& e = m_search_entries[static_cast<size_t>(index)];
    switch (e.category) {
        case SearchCategory::Feature: {
            // 执行命令（含 skill）：经统一命令路径
            if (e.payload >= 0 && e.payload < static_cast<int>(m_palette_cmds.size()))
                run_command(m_palette_cmds[static_cast<size_t>(e.payload)].command, "");
            break;
        }
        case SearchCategory::File: {
            // 插入 @路径 到输入框（不发送；用户回车发送）
            auto& fi = agent::global_file_index();
            if (fi.is_ready()) {
                const auto files = fi.search("", 15);
                if (e.payload >= 0 && e.payload < static_cast<int>(files.size())) {
                    const auto& f = files[static_cast<size_t>(e.payload)];
                    m_input_buffer += "@" + f.relative_path;
                    m_composer_cursor = m_input_buffer.size();
                }
            }
            break;
        }
        case SearchCategory::Session:
            if (e.payload >= 0 && e.payload < static_cast<int>(m_session_metas.size())) {
                const auto& s = m_session_metas[static_cast<size_t>(e.payload)];
                resume_session(s.file_path, s.title);
            }
            break;
        case SearchCategory::Setting:
            run_setting(e.payload);
            break;
    }
}

void App::run_setting(int action) {
    switch (static_cast<SettingAction>(action)) {
        case SettingAction::PermCycle:
            if (m_deps.session) {
                m_deps.session->toggle_permission_mode();
                m_vm.sidebar.permission = mode_label(m_deps.session->permission_mode());
            } else {
                static const char* kCycle[] = {"", "plan", "bypass"};
                m_mock_perm_cycle = (m_mock_perm_cycle + 1) % 3;
                m_vm.sidebar.permission = kCycle[m_mock_perm_cycle];
            }
            m_vm.apply(ActionPermissions{.label = m_vm.sidebar.permission});
            break;
        case SettingAction::ModelSelector:
            open_model_selector();
            break;
        case SettingAction::ProviderSelector:
            open_provider_palette();
            break;
        case SettingAction::ToggleThinking:
            if (!m_vm.messages.empty()) {
                auto& m = m_vm.messages.back();
                if (m.reasoned) {
                    m.reasoning_expanded = !m.reasoning_expanded;
                    m_screen.RequestAnimationFrame();
                }
            }
            break;
        case SettingAction::ToggleSidebar:
            m_sidebar_left = !m_sidebar_left;
            m_screen.RequestAnimationFrame();
            break;
        case SettingAction::Clear:
            if (m_deps.session) m_deps.session->clear_history();
            m_vm.messages.clear();
            invalidate_msg_cache();
            m_scroll = 0;
            break;
        case SettingAction::Exit:
            m_vm.apply(ActionShutdown{});
            if (m_vm.pending_exit) m_screen.Exit();
            break;
    }
}

void App::send_input(const std::string& text) {
    // 本地命令：不发送给模型
    if (!text.empty() && text[0] == '/') {
        // B2 统一命令：斜杠命令全部经 run_command 执行（单一命令路径）
        run_command(text, "");
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

    // 真实链路：统一经 on_submit 路由到会话（B2：输入链单一入口）
    if (m_deps.on_submit) {
        m_deps.on_submit(text);
        // 真实链路唤醒：busy 已置位但动画线程镜像同步发生在 drain（Custom 事件）。
        // 立即投递一次 Custom 让 UI 线程消费事件队列并唤醒动画线程；
        // 否则 publish_async 事件积压（事件总线仅入队、无泵线程），
        // 界面停在"生成中"直到用户再次按键（而按键也不触发 drain）。
        m_screen.PostEvent(Event::Custom);
    } else {
        m_vm.apply(ActionSetBusy{.busy = false});
    }
}

void App::run_command(const std::string& cmd, const std::string& args) {
    // 组装完整命令串（args 已含空格分隔；若调用方传了原始文本则整体处理）
    std::string input = args.empty() ? cmd : cmd + " " + args;
    if (!m_command_processor) {
        m_vm.apply(ActionAppendMessage{.role = "assistant",
                                       .text = std::string(str::kProcessorUnavailable)});
        return;
    }

    agent::command::CommandContext ctx;
    auto result = m_command_processor->process(input, ctx);

    if (result.is_error) {
        m_vm.apply(ActionError{.message = result.output_text});
        return;
    }
    // 本地命令有输出文本 → 追加为 assistant 消息（不回显到模型，token 统计为 0）
    if (!result.output_text.empty() && !result.should_query) {
        m_vm.apply(ActionAppendMessage{.role = "assistant", .text = result.output_text});
        return;
    }
    // 需要调模型：交给 on_submit 路由到会话（与普通文本一致）
    if (result.should_query && m_deps.on_submit) {
        std::string query = result.output_text;
        if (query.empty()) {
            for (const auto& m : result.messages) {
                if (!query.empty()) query += "\n\n";
                query += m;
            }
        }
        if (!query.empty()) {
            m_vm.apply(ActionAppendMessage{.role = "user", .text = query});
            m_vm.apply(ActionSetBusy{.busy = true});
            m_deps.on_submit(query);
            m_screen.PostEvent(Event::Custom);  // 同 send_input：唤醒事件循环消费积压事件
        }
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
        // 结束：封口并回到 IDLE（reasoning_ms 带思考耗时）
        m_queue.push(ActionTurnDone{.full_content = reply, .prompt_ms = think_ms,
                                    .reasoning_ms = think_ms});
        m_screen.PostEvent(Event::Custom);
        log_run("mock: turn done");
        m_stream_run = false;
        log_run("mock: stream exit");
    });
}

// ---------------------------------------------------------------------------
// 转录布局（A3：单一布局源 — 高度估算与渲染共用 estimate_message_height）
// ---------------------------------------------------------------------------

namespace {

/// @brief 定高留白（O(1)：仅占位，不进行 Markdown 解析/高亮）
ftxui::Element pad_rows(int rows) {
    return ftxui::emptyElement() | ftxui::size(ftxui::HEIGHT, ftxui::EQUAL, std::max(0, rows));
}

/// @brief 内容敏感指纹：混入消息/工具参数的实际内容，杜绝"长度相同但内容不同"
///        的 sealed 元素缓存误命中。正文/思考/工具名/参数做全量哈希，工具结果
///        大文本做均匀采样，平衡正确性与长文本开销。
std::uint64_t content_fingerprint(const MessageNode& m) {
    std::uint64_t h = 1469598103934665603ull;
    const auto mix = [&h](std::uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    const auto mix_full = [&](const std::string& s) {
        mix(s.size());
        for (unsigned char c : s) mix(c);
    };
    const auto mix_sample = [&](const std::string& s) {
        mix(s.size());
        const std::size_t n = s.size();
        if (n == 0) return;
        const auto* p = reinterpret_cast<const unsigned char*>(s.data());
        const std::size_t step = std::max<std::size_t>(1, n / 32);
        for (std::size_t i = 0; i < n; i += step) mix(p[i]);
        mix(p[n - 1]);
    };
    mix_full(m.text);
    mix_full(m.reasoning);
    for (const auto& t : m.tool_calls) {
        mix_full(t.tool_name);
        mix_full(t.call_id);
        mix_full(t.arguments);
        mix_sample(t.result);
    }
    return h;
}

}  // namespace

void App::invalidate_msg_cache() {
    m_msg_cache.clear();
    m_msg_height.clear();
    m_msg_height_ver.clear();
}

std::uint64_t App::height_fingerprint(const MessageNode& m) {
    // 与 estimate_message_height 相关的字段；不含宽度（高度与宽度无关）。
    std::uint64_t h = 1469598103934665603ull;
    const auto mix = [&h](std::uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    mix(m.sealed ? 1 : 0);
    mix(m.reasoned ? 1 : 0);
    mix(m.reasoning_expanded ? 1 : 0);
    mix((std::uint64_t)m.text.size());
    mix((std::uint64_t)m.reasoning.size());
    for (const auto& t : m.tool_calls) {
        mix(t.done ? 1 : 0);
        mix(t.running ? 1 : 0);
        mix(t.is_error ? 1 : 0);
        mix(t.expanded ? 1 : 0);
        mix((std::uint64_t)t.arguments.size());
        mix((std::uint64_t)t.result.size());
    }
    return h;
}

std::uint64_t App::sealed_cache_key(const MessageNode& m, int width) {
    // 渲染指纹 = 高度指纹再并入宽度（resize 时元素需重建）
    std::uint64_t h = height_fingerprint(m) ^ (std::uint64_t)(std::uint32_t)width;
    // 混入内容指纹：防止「长度/折叠相同但内容不同」的消息复用旧渲染树
    // （旧树的 reflect 卡片 box 指向失效位置，曾在工具调用场景触发悬空崩溃）
    h ^= content_fingerprint(m);
    // 确保与「无宽度」的高度指纹严格区分，避免高度缓存误命中元素缓存
    return h ^ 0x6a09e667f3bcc909ull;
}

Element App::build_transcript(int width) {
    const auto& msgs = m_vm.messages;
    const std::size_t n = msgs.size();
    if (m_msg_cache.size() < n) m_msg_cache.resize(n);
    if (m_msg_height.size() < n) {
        m_msg_height.resize(n, -1);
        m_msg_height_ver.resize(n, 0);
    }

    // 逐消息估计高度：sealed 消息复用缓存（指纹未变跳过 estimate），
    // 流式消息逐帧重估。再据此线性扫一遍前缀和（O(n) 整数累加，远轻于
    // 逐条 Markdown 解析）。prefix[k] = 前 k 条消息（含每条后的 1 行间距）。
    // 注：正文按显示列宽折行，高度随 width 变化；sealed 高度指纹混入 width，
    //     终端 resize 时自动重算（元素缓存 sealed_cache_key 亦含 width）。
    for (std::size_t i = 0; i < n; ++i) {
        const auto& m = msgs[i];
        if (!m.sealed) {
            m_msg_height[i] = estimate_message_height(m, width);
            m_msg_height_ver[i] = 0;
        } else {
            const std::uint64_t fp = height_fingerprint(m)
                ^ static_cast<std::uint64_t>(width);
            if (m_msg_height_ver[i] != fp) {
                m_msg_height[i] = estimate_message_height(m, width);
                m_msg_height_ver[i] = fp;
            }
        }
    }
    std::vector<int> prefix(n + 1, 0);
    for (std::size_t i = 0; i < n; ++i)
        prefix[i + 1] = prefix[i] + m_msg_height[i] + 1;

    int dimy = ftxui::Terminal::Size().dimy;
    // 减去：顶栏 1 + 输入区 kComposerHeight + 状态行 1 + 余量 2
    int avail = std::max(1, dimy - (1 + 1 + kComposerHeight + 2));
    int content_h = prefix[n];
    int max_scroll = std::max(0, content_h - avail);

    // 确定视口顶行：跟随则钉底部，手动则钳制到 [0, max_scroll]
    int scroll_top;
    if (m_follow) {
        scroll_top = max_scroll;
    } else {
        m_scroll = std::max(0, std::min(m_scroll, max_scroll));
        scroll_top = m_scroll;
        if (m_scroll >= max_scroll) {
            // 已滚到底部：恢复自动跟随（新消息到来时继续钉在底部）
            m_follow = true;
        }
    }

    // 只装配可视切片：顶/底用定高留白补齐，保持总高度与滚动范围不变
    m_hits.clear();
    const int margin = std::max(8, avail / 2);
    TranscriptSlice slice = select_transcript_slice(prefix, scroll_top, avail, margin);

    Element content;
    if (slice.empty) {
        // 空转录或卷过底部：整段留白，保持内容高度以维持滚动范围
        content = pad_rows(content_h);
    } else {
        Elements es;
        if (slice.top_pad > 0) es.push_back(pad_rows(slice.top_pad));
        for (std::size_t i = slice.first; i <= slice.last; ++i) {
            const auto& m = msgs[i];
            const std::size_t hit_start = m_hits.size();

            ftxui::Element msg_el;
            const std::deque<CardHit>* cached_hits = nullptr;
            if (m.sealed) {
                // sealed：按指纹复用渲染树，跳过 Markdown 解析/语法高亮
                const auto key = sealed_cache_key(m, width);
                auto& c = m_msg_cache[i];
                if (c.has && c.width == width && c.key == key) {
                    msg_el = c.element;
                    cached_hits = c.hits.get();
                } else {
                    if (!c.hits) c.hits = std::make_unique<std::deque<CardHit>>();
                    c.hits->clear();
                    c.element = build_message(m, width, 0, c.hits.get());
                    c.width = width;
                    c.key = key;
                    c.has = true;
                    msg_el = c.element;
                    cached_hits = c.hits.get();
                }
            } else {
                // 流式未封口：逐帧重建（命中反射当前帧坐标）
                msg_el = build_message(m, width, m_anim_frame, &m_hits);
            }

            if (!msg_el) {
                // 取证：正常情况下不会为空；一旦触发说明某条消息的渲染树失效。
                // 记录现场后用 emptyElement 兜底，避免布局期解引用空节点崩溃。
                LOG_WARN("[transcript] msg_el null! i={} sealed={} stream={} text_len={} "
                         "reasoning_len={} tool_calls={} width={} m_anim_frame={}",
                         i, m.sealed, m.streaming, m.text.size(), m.reasoning.size(),
                         m.tool_calls.size(), width, m_anim_frame);
                msg_el = ftxui::emptyElement();
            }
            es.push_back(ftxui::hbox({
                ftxui::text("  "),
                ftxui::flex(std::move(msg_el)),
            }));

            if (cached_hits) {
                // 缓存卡片的 box 由 reflect 于渲染期回写，滚动时坐标随之刷新
                for (const auto& h : *cached_hits) {
                    CardHit c = h;
                    c.msg_idx = static_cast<int>(i);
                    m_hits.push_back(c);
                }
            } else {
                for (std::size_t k = hit_start; k < m_hits.size(); ++k)
                    m_hits[k].msg_idx = static_cast<int>(i);
            }
            // 消息间空一行；与 prefix 的「每条 +1 间距」对齐
            es.push_back(ftxui::text(" "));
        }
        if (slice.bottom_pad > 0) es.push_back(pad_rows(slice.bottom_pad));
        content = ftxui::vbox(std::move(es));
    }

    if (m_follow) {
        // 跟随：钉在实际内容底部（focusPositionRelative 无估算误差）
        m_scroll = max_scroll;  // 维护估计底部，切到手动滚动时平滑衔接
        return content | ftxui::focusPositionRelative(0, 1.0f) | ftxui::yframe;
    }

    // 手动：+avail/2 抵消 frame 的居中，使 m_scroll 即视口顶行（增大=向下=更新）
    return content
        | ftxui::focusPosition(0, m_scroll + avail / 2)
        | ftxui::yframe;
}

Element App::build_ask_modal() const {
    if (!m_ask_active) return ftxui::emptyElement();
    if (m_ask_qindex >= m_ask_questions.size()) return ftxui::emptyElement();

    const AskQuestion& q = m_ask_questions[m_ask_qindex];
    const std::string title =
        q.question.empty() ? std::string(str::kAskTitleFallback) : q.question;
    const std::string progress = std::string(str::kAskProgressPrefix) +
                                 std::to_string(m_ask_qindex + 1) +
                                 std::string(str::kAskProgressSep) +
                                 std::to_string(m_ask_questions.size());

    Elements body;
    body.push_back(ftxui::hbox({
        ftxui::text(std::string(str::kAskIcon)),
        ftxui::text(progress) | ftxui::color(theme::T::TextFaint),
        ftxui::separatorEmpty(),
        ftxui::flex(ftxui::text(title) | ftxui::bold),
    }));
    body.push_back(ftxui::separatorEmpty());

    if (m_ask_custom) {
        // 自定义输入模式
        body.push_back(ftxui::hbox({ftxui::text(std::string(str::kAskCustomHint))
                                        | ftxui::color(theme::T::Text)}));
        body.push_back(ftxui::hbox({
            ftxui::text("  "),
            ftxui::flex(m_ask_input->Render()),
        }));
    } else {
        // 选项列表
        for (size_t i = 0; i < q.options.size(); ++i) {
            const bool sel = (static_cast<int>(i) == m_ask_sel);
            std::string marker;
            if (q.multi) {
                marker = (i < m_ask_checked.size() && m_ask_checked[i])
                             ? std::string(str::kAskChecked)
                             : std::string(str::kAskUnchecked);
            } else {
                marker = sel ? std::string(str::kAskCursor) : "  ";
            }
            auto row = ftxui::hbox({
                ftxui::text("  " + marker),
                ftxui::flex(ftxui::text(q.options[i]) | ftxui::color(theme::T::Text)),
            });
            if (sel) row = row | ftxui::bgcolor(theme::T::Selection);
            body.push_back(row);
        }
        if (q.allow_custom_input) {
            const bool sel = (static_cast<int>(q.options.size()) == m_ask_sel);
            auto row = ftxui::hbox({
                ftxui::text(sel ? "  ❯ " : "    "),
                ftxui::flex(ftxui::text(std::string(str::kAskCustomOption))
                                | ftxui::color(theme::T::TextDim)),
            });
            if (sel) row = row | ftxui::bgcolor(theme::T::Selection);
            body.push_back(row);
        }
        body.push_back(ftxui::separatorEmpty());
        body.push_back(ftxui::hbox({
            ftxui::text(q.multi ? std::string(str::kAskMultiHint)
                                : std::string(str::kAskSingleHint))
                | ftxui::color(theme::T::TextFaint),
        }));
    }

    return ftxui::xflex(ftxui::border(ftxui::vbox(std::move(body))));
}

// ---------------------------------------------------------------------------
// 主循环
// ---------------------------------------------------------------------------

void App::run() {
    // 启动即写运行日志：确保每次运行都创建 ~/.workx/logs/codex_run.log，
    // 而非仅在异常/mock 时才落盘（否则正常运行看不到该文件）
    log_run("app start");
#if defined(_WIN32)
    // stdout 为管道（如 opencode 终端）时 FTXUI 尺寸检测回退 80x24，
    // 用 CONOUT$ 兜底校准，避免渲染宽度不足导致右侧露白。
    int w = 0, h = 0;
    if (detect_console_size(w, h)) {
        ftxui::Terminal::SetFallbackSize({w, h});
    }
#endif
    // 冒烟模式（B5）：依赖 mock 流；自动驱动一轮对话后退出
    if (m_deps.smoke_mode) {
        m_deps.mock_mode = true;
        start_smoke_driver();
    }
    // 状态行 / 侧栏：直接构建 Element（装饰性，非聚焦组件）
    auto build_sidebar_elem = [this] {
        return build_sidebar(m_vm.sidebar)
            | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, kSidebarWidth)
            | ftxui::yflex
            | ftxui::bgcolor(theme::T::Panel);
    };
    auto build_status_elem = [this] {
        ftxui::Element line = build_status_line(m_vm.sidebar.model,
                                                m_vm.sidebar.permission,
                                                m_vm.busy);
        // Ctrl+C 提示：单次按下后 1 秒内显示「再次按 Ctrl+C 退出」，超时自动隐藏
        if (m_ctrl_c_hint) {
            if (std::chrono::steady_clock::now() >= m_ctrl_c_hint_until) {
                m_ctrl_c_hint = false;
            } else {
                line = ftxui::hbox({
                    std::move(line),
                    ftxui::text("  "),
                    ftxui::text(std::string(str::kStatusCtrlC))
                        | ftxui::color(theme::T::Accent),
                });
            }
        }
        return line;
    };

    // 命令条目：统一从 agent 命令注册表派生（B2 单一定义，无第二套注册表）
    // 供输入栏提示面板（/ 命令）与聚合搜索面板（功能类）共用
    m_palette_cmds.clear();
    if (m_deps.command_registry) {
        for (const auto& c : m_deps.command_registry->get_user_invocable_commands()) {
            m_palette_cmds.push_back(PaletteCommand{
                .command = "/" + c->name(),
                .title = c->name(),
                .keywords = c->description(),
            });
        }
    }

    ComposerOptions comp_opt;
    comp_opt.buffer = &m_input_buffer;
    comp_opt.cursor = &m_composer_cursor;
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
    comp_opt.on_toggle_thinking = [this] {
        if (!m_vm.messages.empty()) {
            auto& m = m_vm.messages.back();
            if (m.reasoned) {
                m.reasoning_expanded = !m.reasoning_expanded;
                m_screen.RequestAnimationFrame();
            }
        }
    };
    // 输入栏提示面板（/ 命令 · @ 文件）：状态机回调
    comp_opt.suggest_active = [this] { return m_suggest_mode != SuggestMode::None; };
    comp_opt.suggest_move = [this](int delta) { suggest_move(delta); };
    comp_opt.suggest_enter = [this] { return suggest_enter(); };
    comp_opt.suggest_cancel = [this] { suggest_cancel(); };
    comp_opt.suggest_refresh = [this] { update_suggest(); };

    m_composer = make_composer(comp_opt);

    // AskUser 模态输入（始终存在于组件树，按 m_ask_active 显隐）
    m_ask_input = ftxui::Input(&m_ask_buffer, std::string(str::kAskInputPlaceholder));

    // 全局聚合搜索面板（Ctrl+P）：entries 由 App 打开前装配、加载完成后更新，
    // 组件每帧重新过滤（选择位置保留）
    m_palette_comp = make_search_palette(
        m_search_entries,
        [this](int idx) {
            m_palette_open = false;
            apply_search_entry(idx);
            // 选中动作可能已打开新面板（模型/供应商），焦点归新面板；
            // 仅无面板打开时恢复输入栏焦点
            if (!m_model_open && !m_resume_open && !m_provider_open) {
                if (m_composer) m_composer->TakeFocus();
            }
        },
        m_palette_open,
        [this] { if (m_composer) m_composer->TakeFocus(); },
        "", true);

    // /model 模型面板：与搜索面板同款（标题 + 输入框 + Tab 循环；active = 当前模型）
    m_model_comp = make_search_palette(
        m_model_entries,
        [this](int idx) {
            m_model_open = false;
            apply_model(idx);
            if (m_composer) m_composer->TakeFocus();
        },
        m_model_open,
        [this] { if (m_composer) m_composer->TakeFocus(); },
        std::string(str::kPaletteModelTitle));

    // /resume 会话面板：仅会话条目（复用 m_session_metas 缓存）
    m_resume_comp = make_search_palette(
        m_session_entries,
        [this](int idx) {
            m_resume_open = false;
            if (idx >= 0 && idx < static_cast<int>(m_session_metas.size())) {
                const auto& s = m_session_metas[static_cast<size_t>(idx)];
                resume_session(s.file_path, s.title);
            }
            if (m_composer) m_composer->TakeFocus();
        },
        m_resume_open,
        [this] { if (m_composer) m_composer->TakeFocus(); },
        std::string(str::kPaletteResumeTitle));

    // /provider 供应商面板：选择后运行时热切换（后台建后端 → 保留对话换 provider）
    m_provider_comp = make_search_palette(
        m_provider_entries,
        [this](int idx) {
            m_provider_open = false;
            switch_provider(idx);
            if (m_composer) m_composer->TakeFocus();
        },
        m_provider_open,
        [this] { if (m_composer) m_composer->TakeFocus(); },
        std::string(str::kPaletteProviderTitle));

    // 可聚焦组件栈：composer、AskUser 输入、命令面板、模型/会话/供应商面板
    auto container = ftxui::Container::Vertical({
        m_composer,
        m_ask_input,
        m_palette_comp,
        m_model_comp,
        m_resume_comp,
        m_provider_comp,
    });

    auto layout = ftxui::Renderer(container, [&]() -> ftxui::Element {
        try {
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

        // 输出区可用宽度 = 终端宽 − 侧栏（30+1 分隔空格，窄屏折叠则不减）；
        // 再减每行左侧 2 格缩进，作为正文折行的列宽。→ 消息按此宽度折行，
        // 终端 resize 时 width 变化触发元素/高度缓存失效并重排。
        const bool show_sidebar_body = width >= kSidebarCollapseWidth;
        const int sidebar_used = show_sidebar_body ? (kSidebarWidth + 1) : 0;
        const int content_w = std::max(24, width - sidebar_used);
        const int msg_width = std::max(1, content_w - 2);

        Element sidebar_elem = build_sidebar_elem();
        Element left_col = ftxui::flex(build_transcript(msg_width))
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

        // 底部面板：输入区 + 状态行 + 上下各一行空白（看起来内嵌一体）
        Element bottom_panel = ftxui::vbox({
            input_body,
            ftxui::text(" "),  // 状态行上方预留一行空白
            status_elem,
            ftxui::text(" "),  // 状态行下方预留一行空白（带背景色）
        }) | ftxui::bgcolor(theme::T::Panel);

        // 左侧高亮边框线贯穿整个底部面板（输入区 + 空白 + 状态行 + 空白）
        constexpr int kStatusHeight = 3;  // 状态行区域高度（含上下各 1 行空白）
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

        // 输入栏提示面板（/ 命令 · @ 文件）：紧贴输入区上方，激活时占位
        m_suggest_hits.clear();
        Element suggest_elem = render_suggest_panel(
            m_suggest_mode, m_suggest_entries, m_suggest_selected,
            agent::global_file_index().is_ready(), &m_suggest_hits);

        // 左列：标题 + 转录 + 输入区（含内嵌状态行）
        Element content_col = ftxui::vbox({
            ftxui::text(std::string(str::kAppTitle)) | ftxui::color(theme::T::Text),
            left_col | ftxui::yflex,
            build_ask_modal(),
            suggest_elem,
            composer_zone,
        });

        // 侧边栏占满整列（顶到底），输入框右边即为侧边栏。
        // 按 m_sidebar_left 决定侧边栏居中位置（右 / 左）。
        Element content_col_el = content_col | ftxui::flex;
        Element side_space = show_sidebar_body ? ftxui::text(" ") : ftxui::emptyElement();
        Element side_el = show_sidebar_body ? sidebar_elem : ftxui::emptyElement();
        Element body = m_sidebar_left
                           ? ftxui::hbox({side_el, side_space, content_col_el})
                           : ftxui::hbox({content_col_el, side_space, side_el});

        // 面板以居中叠加方式呈现（不压缩内容区）
        // 命令面板：悬浮于主会话之上、不整屏清空背景（四周可见会话内容）
        Elements layers;
        layers.push_back(body);
        // 拖拽复制成功反馈：紧贴转录区底部、透明背景的一行浅字。
        // 超时由本帧（UI 线程）判定后隐藏，避免提示残留。
        if (m_copy_flash) {
            if (std::chrono::steady_clock::now() >= m_copy_flash_until) {
                m_copy_flash = false;
            } else {
                auto copy_msg = ftxui::hbox({
                    ftxui::text("  "),
                    ftxui::text("已复制 " + std::to_string(m_copy_flash_n) + " 字符"),
                }) | ftxui::color(theme::T::Accent) | ftxui::clear_under;
                layers.push_back(ftxui::vbox({ftxui::filler(), copy_msg}));
            }
        }
        if (m_palette_open)
            layers.push_back(ftxui::center(m_palette_comp->Render()));
        if (m_model_open)
            layers.push_back(ftxui::center(m_model_comp->Render()));
        if (m_resume_open)
            layers.push_back(ftxui::center(m_resume_comp->Render()));
        if (m_provider_open)
            layers.push_back(ftxui::center(m_provider_comp->Render()));
        // 整个背景使用黑色
        return ftxui::dbox(std::move(layers)) | ftxui::bgcolor(theme::T::Canvas);
        } catch (...) {
            // 防御：单帧渲染即便抛出（含非 std::exception 类型），也不得令整个程序退出。
            log_run("render exception: (unknown type)");
            return ftxui::text(std::string("渲染异常，重启以恢复正常"))
                   | ftxui::color(theme::T::Accent) | ftxui::bgcolor(theme::T::Canvas);
        }
    });

    auto root = layout | ftxui::CatchEvent([&](Event e) {
        if (e == Event::Custom) {
            drain();
            // 冒烟模式：UI 线程消费 driver 请求（投递消息 / 请求退出）
            if (m_smoke_submit.exchange(false)) {
                send_input("smoke: 渲染与滚动验证");
            }
            if (m_smoke_exit.exchange(false)) {
                m_screen.Exit();
            }
            // 思考动画期间持续推进重绘（动画线程定时 Post Custom）
            if (m_busy) m_screen.RequestAnimationFrame();
            return true;
        }
        if (m_ask_active) {
            // 防御：m_ask_active 与 m_ask_questions 不同步时（如解析后无有效问题），
            // 关闭模态并回填取消结果，避免 m_ask_questions[m_ask_qindex] 越界崩溃。
            if (m_ask_qindex >= m_ask_questions.size()) {
                close_ask(false);
                m_screen.RequestAnimationFrame();
                return true;
            }
            // B3：多问题 AskUser 交互
            // ↑↓ 移动选项；Enter 确认当前题/提交；空格（多选）勾选；
            // Esc 取消（自定义输入模式先返回选项）
            if (m_ask_custom) {
                if (e == Event::Return) { advance_ask(); m_screen.RequestAnimationFrame(); return true; }
                if (e == Event::Escape) { m_ask_custom = false; m_ask_sel = 0; m_ask_buffer.clear(); m_screen.RequestAnimationFrame(); return true; }
            } else {
                const AskQuestion& aq = m_ask_questions[m_ask_qindex];
                const int total = static_cast<int>(aq.options.size())
                                  + (aq.allow_custom_input ? 1 : 0);
                if (e == Event::ArrowUp) {
                    m_ask_sel = (m_ask_sel <= 0) ? total - 1 : m_ask_sel - 1;
                    m_screen.RequestAnimationFrame();
                    return true;
                }
                if (e == Event::ArrowDown) {
                    m_ask_sel = (m_ask_sel >= total - 1) ? 0 : m_ask_sel + 1;
                    m_screen.RequestAnimationFrame();
                    return true;
                }
                if (e == Event::Return) {
                    // 选中自定义输入项 → 进入输入模式；否则记录答案前进
                    if (aq.allow_custom_input &&
                        static_cast<size_t>(m_ask_sel) >= aq.options.size()) {
                        m_ask_custom = true;
                        m_ask_buffer.clear();
                        if (m_ask_input) m_ask_input->TakeFocus();
                    } else {
                        advance_ask();
                    }
                    m_screen.RequestAnimationFrame();
                    return true;
                }
                if (aq.multi && e.character() == " ") {
                    if (m_ask_sel >= 0 && static_cast<size_t>(m_ask_sel) < m_ask_checked.size()) {
                        m_ask_checked[static_cast<size_t>(m_ask_sel)] =
                            !m_ask_checked[static_cast<size_t>(m_ask_sel)];
                    }
                    m_screen.RequestAnimationFrame();
                    return true;
                }
                if (e == Event::Escape) { close_ask(false); m_screen.RequestAnimationFrame(); return true; }
            }
        }
        // Ctrl+P：全局聚合搜索面板（会话 / 文件 / 设置）。
        // 不放在输入栏消费：任意焦点下均可呼出（composer 已不拦截该键）
        {
            const std::string s(1, static_cast<char>(0x10));
            const bool ctrl_p = (e.is_character() && e.character() == s) ||
                                e == ftxui::Event::Special(s);
            if (ctrl_p) {
                // 面板互斥：同一时刻只开一个悬浮面板
                m_model_open = false;
                m_resume_open = false;
                m_provider_open = false;
                // 打开前装配聚合条目（会话列表未加载则后台加载，面板刷新时自动出现）
                m_search_entries = assemble_search_entries();
                m_palette_open = true;
                if (m_palette_comp) m_palette_comp->TakeFocus();
                return true;
            }
        }
        // Esc：打断模型回复 / 工具调用（刷新 / AskUser / 面板的 Esc 已在上方各自处理）
        if (e == Event::Escape) {
            // 任一悬浮面板打开时，Esc 交回面板自身（先清空搜索再关闭），
            // 不在此拦截：否则根 CatchEvent 吞掉事件，面板永远收不到 Esc 而关不掉。
            if (m_palette_open || m_model_open || m_resume_open || m_provider_open)
                return false;
            if (m_busy.load() && m_deps.event_bus)
                m_deps.event_bus->publish(agent::InterruptEvent{.force = false});
            return true;
        }
        // Ctrl+C：单次清空输入栏 + 状态栏提示；1 秒内连按两次 → 打断并退出
        {
            const std::string cs(1, static_cast<char>(0x03));
            const bool ctrl_c = (e.is_character() && e.character() == cs) ||
                                e == ftxui::Event::Special(cs);
            if (ctrl_c) {
                const auto now = std::chrono::steady_clock::now();
                if (m_last_ctrl_c != std::chrono::steady_clock::time_point{} &&
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        now - m_last_ctrl_c).count() <= 1000) {
                    m_last_ctrl_c = {};  // 双击：退出
                    m_ctrl_c_hint = false;
                    if (m_deps.event_bus)
                        m_deps.event_bus->publish(agent::InterruptEvent{.force = true});
                    if (m_deps.on_exit) m_deps.on_exit();
                    else m_screen.Exit();
                    return true;
                }
                m_last_ctrl_c = now;
                // 单次：仅清空输入栏 + 关闭提示面板（打断请用 Esc）
                m_input_buffer.clear();
                m_composer_cursor = 0;
                suggest_cancel();
                // 状态栏提示「再次按 Ctrl+C 退出」（1 秒窗口，渲染期超时自动隐藏）
                m_ctrl_c_hint = true;
                m_ctrl_c_hint_until = now + std::chrono::milliseconds(1000);
                m_screen.RequestAnimationFrame();
                return true;
            }
        }
        if (e.is_mouse()) {
            // 悬浮面板打开时，鼠标事件先转发给面板组件（点击选中 / 滚轮滚动列表）。
            // 面板未消费（如点击面板外）才落到下层转录的滚动/卡片/选中逻辑。
            ftxui::Component active_panel = nullptr;
            if (m_palette_open) active_panel = m_palette_comp;
            else if (m_model_open) active_panel = m_model_comp;
            else if (m_resume_open) active_panel = m_resume_comp;
            else if (m_provider_open) active_panel = m_provider_comp;
            if (active_panel && active_panel->OnEvent(e)) {
                m_screen.RequestAnimationFrame();
                return true;
            }
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
                // 提示面板（/ 命令 · @ 文件）候选行：点击选中并确认
                if (m_suggest_mode != SuggestMode::None && !m_suggest_hits.empty()) {
                    for (std::size_t i = 0; i < m_suggest_hits.size(); ++i) {
                        const auto& b = m_suggest_hits[i];
                        if (e.mouse().x >= b.x_min && e.mouse().x <= b.x_max &&
                            e.mouse().y >= b.y_min && e.mouse().y <= b.y_max) {
                            m_suggest_selected = static_cast<int>(i);
                            suggest_enter();
                            m_screen.RequestAnimationFrame();
                            return true;
                        }
                    }
                }
                for (const auto& hit : m_hits) {
                    if (e.mouse().x >= hit.box.x_min && e.mouse().x <= hit.box.x_max &&
                        e.mouse().y >= hit.box.y_min && e.mouse().y <= hit.box.y_max) {
                        auto& msg = m_vm.messages[static_cast<std::size_t>(hit.msg_idx)];
                        if (hit.button >= 0) {
                            // 消息操作按钮：复制 / 重试
                            if (hit.button == 0) {
                                if (!msg.text.empty() && write_clipboard(msg.text))
                                    flash_copy_message(utf8_char_count(msg.text));
                            } else if (hit.button == 1) {
                                retry_message(hit.msg_idx);
                            }
                        } else if (hit.tool_idx < 0) {
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
            // 拖拽选中完成（左键释放）：把最新选中文本写入系统剪贴板。
            // 若这次按下时点中了折叠卡片（上面已 return true → 选区被 FTXUI
            // 清空），m_selection_text 为空，跳过；否则正常复制。返回 false
            // 交给 FTXUI 保留选区高亮，供用户确认所复制的范围。
            if (e.mouse().button == ftxui::Mouse::Left &&
                e.mouse().motion == ftxui::Mouse::Released) {
                std::string sel = m_selection_text;
                while (!sel.empty() && (sel.back() == ' ' || sel.back() == '\t' ||
                                        sel.back() == '\n' || sel.back() == '\r'))
                    sel.pop_back();  // 选区末尾常带留白/换行，写入剪贴板前去净
                if (!sel.empty() && write_clipboard(sel))
                    flash_copy_message(utf8_char_count(sel));
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

    // 重绘驱动线程（A4：IDLE 零耗）：仅 busy（思考动画/流式）时按 80ms 节奏
    // 驱动帧；IDLE 时完全睡眠（等待 busy 变化唤醒），不再无条件 PostEvent 烧 CPU。
    // 无 tty 时的终端尺寸校准由每帧 Render 内 detect_console_size 完成。
    m_anim_run = true;
    m_anim_thread = std::thread([this] {
        std::unique_lock<std::mutex> lock(m_anim_mutex);
        while (m_anim_run) {
            if (!m_busy.load()) {
                // IDLE：阻塞等待（busy 置位或退出时唤醒）
                m_anim_cv.wait(lock, [this] { return !m_anim_run || m_busy.load(); });
                continue;
            }
            lock.unlock();
            m_screen.PostEvent(Event::Custom);
            std::this_thread::sleep_for(std::chrono::milliseconds(80));
            lock.lock();
        }
    });

    try {
        crash::ReinstallSignalHandlers();
        m_screen.Loop(root);
    } catch (const std::exception& ex) {
        log_run(std::string("loop exception: ") + ex.what());
        throw;
    }
}

// ---------------------------------------------------------------------------
// helper：权限模式标签
// ---------------------------------------------------------------------------

/// @brief 冒烟驱动（B5）：等待首帧后投递一条用户消息（走 mock 流），
///        轮询完成条件（busy 回落且已有 ≥2 条消息），再渲染约 1s 后请求退出。
///        15s 未完成按失败退出（m_exit_code=1）。driver 只经 atomic 与
///        UI 线程通信，不直接触碰 ViewModel。
void App::start_smoke_driver() {
    std::thread([this] {
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        m_smoke_submit.store(true);
        m_screen.PostEvent(Event::Custom);

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        bool done = false;
        while (std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!m_busy.load() && m_msg_count.load() >= 2) { done = true; break; }
        }
        // 完成后再渲染约 1s（思考动画/状态行/滚动路径均过一遍）
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        m_exit_code.store(done ? 0 : 1);
        m_smoke_exit.store(true);
        m_screen.PostEvent(Event::Custom);
    }).detach();
}

std::string App::mode_label(agent::tool::PermissionMode m) {
    switch (m) {
        case agent::tool::PermissionMode::Plan: return "plan";
        case agent::tool::PermissionMode::BypassPermissions: return "bypass";
        default: return "";
    }
}

// ---------------------------------------------------------------------------
// 拖拽选中 → 复制剪贴板（FTXUI 原生 Selection + 系统剪贴板）
// ---------------------------------------------------------------------------

/// @brief 选区文本变化回调：SelectionChange 由 FTXUI 在每次绘制后、且选区实际
///        变化时调用（主 loop 线程）。这里缓存最新选中文本，供鼠标释放时复制。
void App::on_selection_changed() {
    m_selection_text = m_screen.GetSelection();
}

/// @brief 显示「已复制 N 字符」短暂提示。自清除线程只 sleep 后触发一次全局重绘
///        （ftxui::animation::RequestAnimationFrame），不触碰任何 App 成员，
///        因此可安全 detach/join；超时判定由主线程在渲染时完成。
void App::flash_copy_message(std::size_t char_count) {
    m_copy_flash_n = char_count;
    m_copy_flash = true;
    m_copy_flash_until = std::chrono::steady_clock::now() +
                         std::chrono::milliseconds(1500);
    if (m_copy_flash_thread.joinable()) m_copy_flash_thread.detach();
    m_copy_flash_thread = std::thread([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        ftxui::animation::RequestAnimationFrame();  // 全局触发重绘，清除提示
    });
}

/// @brief 重试指定助手消息：截断到其触发用户消息并重新生成
void App::retry_message(int msg_idx) {
    if (m_vm.busy) return;  // 生成中不重试
    // 找到该助手消息之前的最后一条用户消息（触发该回复的 prompt）
    int user_idx = -1;
    for (int i = msg_idx - 1; i >= 0; --i) {
        if (m_vm.messages[static_cast<std::size_t>(i)].role == MsgRole::User) {
            user_idx = i;
            break;
        }
    }
    if (user_idx < 0) return;
    const std::string user_text = m_vm.messages[static_cast<std::size_t>(user_idx)].text;
    // 截断 VM：删除该助手消息及其后所有消息（保留触发它的用户消息）
    m_vm.messages.erase(m_vm.messages.begin() + msg_idx, m_vm.messages.end());
    invalidate_msg_cache();
    m_follow = true;
    if (m_deps.mock_mode) {
        start_mock_stream(user_text);
        m_screen.PostEvent(Event::Custom);
        return;
    }
    if (!m_deps.session) return;
    // 会话侧：截断到该用户消息并重新生成
    m_deps.session->regenerate_from(user_text);
    m_vm.apply(ActionSetBusy{.busy = true});
    m_screen.PostEvent(Event::Custom);  // 唤醒事件循环消费积压事件
}

}  // namespace ftxtui