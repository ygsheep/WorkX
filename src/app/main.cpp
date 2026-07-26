/**
 * @file main.cpp
 * @brief Workx TUI 入口
 * @details 解析参数，组装各层，运行
 * @version 3.1.0
 * @date 2026-07
 */

#include <algorithm>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <string>

#include <liblogger/logger.h>

#include "agent/api/backend_factory.h"
#include "agent/api/chat_types.h"
#include "agent/api/i_backend.h"
#include "agent/command/inclaude/registry.h"
#include "agent/input/processor.h"
#include "agent/core/chat_session.h"
#include "agent/message/types.h"
#include "agent/model/provider_preset.h"
#include "agent/model/context_resolver.h"
#include "agent/tool/FileEditTool/file_edit_tool.h"
#include "agent/tool/FileReadTool/file_read_tool.h"
#include "agent/tool/FileWriteTool/file_write_tool.h"
#include "agent/tool/registry.h"
#include "app/command/builtin_commands.h"
#include "app/config/app_config.h"
#include "app/config/cli_args.h"
#include "app/ui/model_selector.h"
#include "app/ui/path_completer.h"
#include "app/ui/file_index.h"
#include "core/config/config_manager.h"
#include "core/events/event_bus.h"
#include "core/task/task_manager.h"
#include "tui/core/platform/i_platform.h"
#include "tui/core/screen.h"
#include "tui/core/terminal.h"
#include "tui/render/chat_renderer.h"
#include "tui/setup/setup_wizard.h"
#include "tui/widgets/bottom_bar_manager.h"
#include "tui/widgets/command_panel.h"
#include "tui/widgets/status_bar.h"

namespace agent {

// ============================================================
// 主函数
// ============================================================

static int run(int argc, char* argv[]) {
    // ---- 注册配置元数据 ----
    register_config_defaults();

    // ---- 配置加载顺序：配置文件 → 环境变量 → CLI 参数 ----
    // 1. 配置文件（最低优先级）
    // 先检查 --config 参数
    std::filesystem::path explicit_config_path;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--config" && i + 1 < argc) {
            explicit_config_path = argv[++i];
        }
    }

    if (!explicit_config_path.empty()) {
        load_from_config_file(explicit_config_path);
    } else {
        // 尝试默认配置文件
        auto default_path = default_config_path();
        if (std::filesystem::exists(default_path)) {
            load_from_config_file(default_path);
        }
    }

    // 2. 环境变量（中等优先级）
    load_from_env();

    // 3. CLI 参数（最高优先级）
    parse_cli_args(argc, argv);

    // ---- 从 ConfigManager 读取配置 ----
    auto& cfg = ConfigManager::instance();

    // ---- 初始化日志系统 ----
    {
        auto logger = Agent::Logger::get_instance();

        // 设置日志级别
        std::string level_str = cfg.get_or<std::string>(keys::LOG_LEVEL, "info");
        std::transform(level_str.begin(), level_str.end(), level_str.begin(), ::tolower);
        if (level_str == "trace")       logger->set_level(Agent::LogLevel::TRACE);
        else if (level_str == "debug")  logger->set_level(Agent::LogLevel::DEBUG);
        else if (level_str == "warn")   logger->set_level(Agent::LogLevel::WARN);
        else if (level_str == "error")  logger->set_level(Agent::LogLevel::ERROR);
        else if (level_str == "fatal")  logger->set_level(Agent::LogLevel::FATAL);
        else                            logger->set_level(Agent::LogLevel::INFO);

        // 启用文件输出（默认写入 %APPDATA%/workx/logs/workx.log）
        std::string log_file = cfg.get_or<std::string>(keys::LOG_FILE, "");
        if (log_file.empty()) {
            log_file = default_log_path().string();
        }
        logger->enable_file_output(log_file, true);
    }

    // ---- Debug 启动信息 ----
    bool verbose = cfg.get_or<bool>(keys::VERBOSE, false);
    if (verbose) {
        std::cerr << "[debug] Config loaded\n";
        std::cerr << "[debug]   provider:  " << cfg.get_or<std::string>(keys::PROVIDER, "(not set)") << "\n";
        std::cerr << "[debug]   api_key:   " << (cfg.get_or<std::string>(keys::API_KEY, "").empty() ? "(empty)" : "***") << "\n";
        std::cerr << "[debug]   remote:    " << cfg.get_or<std::string>(keys::REMOTE_URL, "(not set)") << "\n";
        std::cerr << "[debug]   model:     " << cfg.get_or<std::string>(keys::MODEL_NAME, "(not set)") << "\n";
        std::cerr << "[debug]   simple_io: " << (cfg.get_or<bool>(keys::SIMPLE_IO, false) ? "true" : "false") << "\n";
    }

    // ---- Terminal ----
    TerminalConfig config;
    config.simple_io = cfg.get_or<bool>(keys::SIMPLE_IO, false);
    config.use_color = !cfg.get_or<bool>(keys::NO_COLOR, false);
    config.prompt_string = cfg.get_or<std::string>(keys::PROMPT, "> ");

    Terminal terminal(config);

    auto init_result = terminal.initialize();
    if (init_result.isErr()) {
        std::cerr << "Failed to initialize terminal: " << init_result.error() << "\n";
        terminal.restore();
        return 1;
    }

    // ---- 文件索引构建（TUI 启动时扫描工作目录）----
    {
        namespace fs = std::filesystem;
        std::string cwd = fs::current_path().string();
        auto& index = global_file_index();
        index.build(cwd);
        if (verbose) {
            std::cerr << "[debug] File index built: " << index.size() << " files\n";
        }
    }

    // ---- 首次运行向导 ----
    // 当 provider、api_key、remote 都没配置时，启动交互式设置向导
    bool needs_wizard = cfg.get_or<std::string>(keys::PROVIDER, "").empty()
                     && cfg.get_or<std::string>(keys::API_KEY, "").empty()
                     && cfg.get_or<std::string>(keys::REMOTE_URL, "").empty();
    // ---- 差分渲染引擎（供向导等交互式 UI 使用） ----
    Screen screen(&terminal);

    if (needs_wizard) {
        SetupWizard wizard(terminal.platform(), &terminal, &screen);
        bool ok = wizard.run_wizard();
        if (!ok) {
            terminal.restore();
            return 0;
        }
        // Screen 内部已通过 clear() + flush() 清空虚拟屏幕
    }

    // ---- Backend (可选) ----
    std::unique_ptr<ChatSession> session;
    std::shared_ptr<command::CommandRegistry> registry;
    std::unique_ptr<agent::input::InputProcessor> input_processor;

    // 检查 Provider Preset（--provider 时自动填充 URL/Model）
    std::string provider_name = cfg.get_or<std::string>(keys::PROVIDER, "");
    const ProviderPreset* preset = provider_name.empty() ? nullptr : find_preset(provider_name);

    // URL: --remote(显式设置) > preset > ""
    std::string remote_url;
    if (cfg.has(keys::REMOTE_URL)) {
        remote_url = cfg.get_or<std::string>(keys::REMOTE_URL, "");
    } else if (preset && !preset->default_url.empty()) {
        remote_url = std::string(preset->default_url);
    }

    // Model: --model(显式设置) > preset > ""
    std::string model_name;
    if (cfg.has(keys::MODEL_NAME)) {
        model_name = cfg.get_or<std::string>(keys::MODEL_NAME, "");
    } else if (preset && !preset->default_model.empty()) {
        model_name = std::string(preset->default_model);
    }

    if (!remote_url.empty()) {
        BackendConfig backend_config;
        backend_config.type = BackendConfig::Type::Remote;
        backend_config.provider = preset ? preset->type : ProviderType::OpenAI;
        backend_config.base_url = remote_url;
        backend_config.model_name = model_name;
        backend_config.api_key = cfg.get_or<std::string>(keys::API_KEY, "");
        int default_timeout = preset && preset->timeout_ms > 0 ? preset->timeout_ms : 30000;
        backend_config.timeout_ms = cfg.get_or<int>(keys::TIMEOUT_MS, default_timeout);

        auto backend = BackendFactory::create(backend_config);
        if (!backend) {
            std::cerr << "Failed to create backend\n";
            terminal.restore();
            return 1;
        }

        auto backend_init_result = backend->initialize(backend_config);
        if (backend_init_result.isErr()) {
            std::cerr << "Failed to initialize backend: " << backend_init_result.error() << "\n";
            terminal.restore();
            return 1;
        }

        int default_retry_delay = preset && preset->retry_delay_ms > 0 ? preset->retry_delay_ms : 1000;
        // D-1：显式组装依赖 — 三大核心组件均通过单例注入，
        // 未来切换 Mock 时只需修改这三处引用
        session = std::make_unique<ChatSession>(
            std::move(backend), default_retry_delay, "default",
            TaskManager::instance(),
            EventBus::instance(),
            ConfigManager::instance());

        if (verbose) {
            std::cerr << "[debug] Backend ready\n";
            std::cerr << "[debug]   provider: " << (preset ? preset->name : "openai") << "\n";
            std::cerr << "[debug]   url:      " << remote_url << "\n";
            std::cerr << "[debug]   model:    " << model_name << "\n";
        }

        // 注册工具到 ToolRegistry（启用 function calling）
        auto tool_registry = std::make_shared<agent::tool::ToolRegistry>();
        tool_registry->register_tool(std::make_shared<agent::tool::FileReadTool>());
        tool_registry->register_tool(std::make_shared<agent::tool::FileWriteTool>());
        tool_registry->register_tool(std::make_shared<agent::tool::FileEditTool>());
        session->set_tool_registry(tool_registry);

        // 设置系统提示词（拼接工具 prompt，让 LLM 知道如何使用工具）
        std::string sys_prompt = cfg.get_or<std::string>(keys::SYSTEM_PROMPT, "");
        for (const auto& t : tool_registry->get_all_tools()) {
            sys_prompt += "\n\n";
            sys_prompt += t->prompt();
        }
        // @file 引用说明：用户消息中的 <file path="..."> 标签由前端预处理注入，
        // 已包含文件完整内容。LLM 不应再对其中路径调用 Read 工具，避免重复读取。
        sys_prompt +=
            "\n\n"
            "用户消息中可能出现 <file path=\"...\">...</file> 标签，这是用户通过 "
            "@path 语法引用的文件内容，已由前端读取并注入。对此类标签内的路径，"
            "禁止再次调用 Read 工具读取；直接基于标签内已有内容回答用户问题。";
        if (!sys_prompt.empty()) {
            session->set_system_prompt(sys_prompt);
        }
    }

    // ---- 命令系统 ----
    registry = std::make_shared<command::CommandRegistry>();
    input_processor = std::make_unique<agent::input::InputProcessor>(registry);

    // ---- 启动时模型选择（model_name 为空时触发） ----
    if (session && session->backend() && model_name.empty()) {
        ModelSelection sel = select_model_interactive(
            &terminal, &screen, session->backend(), model_name);
        if (!sel.name.empty()) {
            cfg.set(keys::MODEL_NAME, sel.name);
            session->backend()->set_model_name(sel.name);
            model_name = sel.name;
            // 持久化用户选择的 context_length（若 selector 返回了有效值）
            if (sel.context_length > 0) {
                cfg.set(keys::CONTEXT_LENGTH, static_cast<int>(sel.context_length));
            }
            cfg.save_to_file(default_config_path());
        }
    }

    // ---- Tab 补全 ----
    terminal.set_completion_callback(
        [&registry](std::string_view line, size_t cursor_pos)
            -> std::vector<std::pair<std::string, size_t>> {
            std::vector<std::pair<std::string, size_t>> results;

            if (!line.empty() && line[0] == '/') {
                std::string prefix(line.substr(1, cursor_pos - 1));
                if (registry) {
                    for (const auto& cmd : registry->get_user_invocable_commands()) {
                        if (cmd->name().compare(0, prefix.size(), prefix) == 0) {
                            std::string completion = "/" + cmd->name();
                            results.push_back({completion + " ", completion.size() + 1});
                        }
                    }
                }
                return results;
            }

            std::string_view before_cursor = line.substr(0, cursor_pos);
            if (before_cursor.find('/') != std::string_view::npos
#ifdef _WIN32
                || before_cursor.find('\\') != std::string_view::npos
#endif
            ) {
                return complete_file_path(line, cursor_pos);
            }

            return results;
        }
    );

    // ---- ChatRenderer ----
    ChatRenderer renderer(&terminal);
    renderer.start();

    // ---- Ctrl+O 回调：切换思考视图 ----
    terminal.set_ctrl_o_callback([&renderer]() {
        renderer.toggle_thinking_view();
    });

    // ---- BottomBarManager 初始化 ----
    auto& bottom_bar = terminal.bottom_bar();
    bottom_bar.initialize(&screen);

    // StatusBar 初始化（通过 BottomBarManager）
    // 将 ChatRenderer 的 StatusBar 注入 BottomBarManager，避免两个实例不同步
    if (auto* sb = renderer.status_bar()) {
        bottom_bar.set_status_bar(sb);
        sb->set_model_name(model_name.empty() ? "unknown" : model_name);
        namespace fs = std::filesystem;
        sb->set_project_name(fs::current_path().filename().string());

        // 上下文窗口：统一通过 resolver 解析（优先级：provider→user cfg→capability→preset→default）
        // 启动初始化时无 selector 返回值，sel_context_length 传 0；
        // 若启动时已通过 select_model_interactive 选择模型并持久化了 context_length，
        // 这里读 cfg 即可拿到（来源为 ProviderList 的值已 save_to_file）
        auto resolution = resolve_context_length(
            model_name,
            /*sel_context_length=*/0,
            cfg.get_or<int>(keys::CONTEXT_LENGTH, 0),
            preset);
        sb->set_context_limit(resolution.value);
    }

    // 注册内置系统命令（help/exit/quit/clear/regen/model）
    command::SystemCommandContext sys_ctx;
    sys_ctx.session = session.get();
    sys_ctx.on_exit = []() {
        EventBus::instance().publish(ShutdownEvent{.force = false});
    };
    sys_ctx.on_model_select = [&terminal, &screen, &session, &cfg, &renderer, &preset]() {
        if (!session || !session->backend()) {
            terminal.set_color(ColorRole::Error);
            terminal.write("No backend configured. Use --provider first.\n");
            terminal.reset_color();
            return;
        }
        ModelSelection sel = select_model_interactive(
            &terminal, &screen, session->backend(),
            cfg.get_or<std::string>(keys::MODEL_NAME, ""));
        if (!sel.name.empty()) {
            cfg.set(keys::MODEL_NAME, sel.name);
            if (session->backend()) session->backend()->set_model_name(sel.name);
            if (auto* sb = renderer.status_bar()) {
                sb->set_model_name(sel.name);
                // 上下文窗口：统一通过 resolver 解析（优先级：provider→user cfg→capability→preset→default）
                auto resolution = resolve_context_length(
                    sel.name,
                    sel.context_length,
                    cfg.get_or<int>(keys::CONTEXT_LENGTH, 0),
                    preset);
                sb->set_context_limit(resolution.value);
                // 仅当来源是 ProviderList 时持久化（避免用兜底值覆盖用户配置）
                if (resolution.source == ContextLengthResolution::Source::ProviderList) {
                    cfg.set(keys::CONTEXT_LENGTH, static_cast<int>(resolution.value));
                }
            }
            cfg.save_to_file(default_config_path());
            terminal.set_color(ColorRole::System);
            terminal.write(std::format("Model set to: {}\n", sel.name));
            terminal.reset_color();
        }
    };
    command::register_system_commands(*registry, sys_ctx);

    // CommandPanel 初始化（从 CommandRegistry 获取命令列表）
    // registry 由 make_shared 创建，保证非空，无需空检查
    {
        std::vector<CommandEntry> entries;
        for (const auto& cmd : registry->get_user_invocable_commands()) {
            const auto& hint = cmd->argument_hint();
            entries.push_back({
                cmd->name(),
                cmd->description(),
                hint.value_or("/" + cmd->name())
            });
        }
        bottom_bar.command_panel().set_commands(entries);
    }

    // ---- 状态栏定期刷新 ----
    terminal.set_status_refresh_callback([&renderer, &bottom_bar]() {
        // 仅在 StatusBar 模式下刷新状态栏；CommandPanel/SelectPanel 模式下跳过
        if (bottom_bar.mode() != BottomBarMode::STATUS_BAR) return;
        if (auto* sb = renderer.status_bar()) {
            // 推进动画帧（仅在非 IDLE 状态）
            if (sb->is_active_state()) {
                sb->advance_frame();
            }
            sb->render();
        }
    });

    // ---- LineEditor 回调：命令面板导航 + Tab 补全 + 输入变化 ----
    terminal.set_command_nav_callback([&bottom_bar](char32_t key) -> bool {
        return bottom_bar.handle_navigation(key);
    });
    terminal.set_command_tab_callback([&bottom_bar]() -> std::string {
        return bottom_bar.handle_tab();
    });
    terminal.set_input_changed_callback([&bottom_bar](const std::string& line) {
        bottom_bar.on_input_changed(line);
    });

    // ---- 事件订阅：统一用户输入管道 ----
    // UserInputEvent 经过 InputParser(解析层) → InputProcessor(处理层)
    // → 根据 ProcessResult 调用执行层组件

    auto input_token = EventBus::instance().subscribe<UserInputEvent>(
        [&session, &input_processor](const UserInputEvent& e) {
            if (!input_processor) return;

            command::CommandContext ctx;
            auto result = input_processor->process(e.text, ctx);

            if (result.is_error) {
                EventBus::instance().publish_async(StreamErrorEvent{
                    .session_id = "default",
                    .message = result.output_text,
                    .retryable = false
                });
                return;
            }

            // 命令有输出文本 → 直接发布
            if (!result.output_text.empty()) {
                EventBus::instance().publish_async(StreamTokenEvent{
                    .session_id = "default",
                    .content_delta = result.output_text,
                    .reasoning_delta = "",
                    .is_thinking = false,
                    .token_count = 0
                });
                EventBus::instance().publish_async(StreamDoneEvent{
                    .session_id = "default",
                    .full_content = result.output_text,
                    .full_reasoning = "",
                    .was_interrupted = false
                });
                return;
            }

            // 需要调 LLM
            if (result.should_query) {
                if (session) {
                    // 使用处理后的文本（已展开 @file 引用为文件内容）
                    std::string query_text;
                    if (!result.messages.empty()) {
                        for (size_t i = 0; i < result.messages.size(); ++i) {
                            if (i > 0) query_text += "\n\n";
                            query_text += result.messages[i];
                        }
                    } else {
                        query_text = e.text;
                    }
                    session->send_message(query_text);
                } else {
                    // 无后端时回显
                    EventBus::instance().publish_async(StreamTokenEvent{
                        .session_id = "default",
                        .content_delta = "Echo: " + e.text + "\n",
                        .reasoning_delta = "",
                        .is_thinking = false,
                        .token_count = 0
                    });
                    EventBus::instance().publish_async(StreamDoneEvent{
                        .session_id = "default",
                        .full_content = "Echo: " + e.text + "\n",
                        .full_reasoning = "",
                        .was_interrupted = false
                    });
                }
            }
        }
    );

    auto shutdown_token = EventBus::instance().subscribe<ShutdownEvent>(
        [&terminal](const ShutdownEvent& /*e*/) {
            terminal.shutdown();
        }
    );

    // ---- 运行主循环 ----
    terminal.run();

    // ---- 清理 ----
    // 顺序：unsubscribe → cancelAll → waitForAll → renderer.stop → clear EventBus → restore
    // 先取消订阅，避免 cancelAll 触发的事件进入已失效的回调
    EventBus::instance().unsubscribe<UserInputEvent>(input_token);
    EventBus::instance().unsubscribe<ShutdownEvent>(shutdown_token);

    // 先取消并等待所有任务，再恢复终端
    // 这样任务完成时发的 UI 事件还能正常处理
    TaskManager::instance().cancelAll();
    TaskManager::instance().waitForAll();

    // 显式停止 ChatRenderer，确保 StreamingBuffer/Spinner 线程在 EventBus
    // 和 Terminal 仍可用时完成 join + flush，避免栈析构时触发 CRT 断点
    renderer.stop();

    // 清空 EventBus 订阅，防止后续异步事件触发已失效的回调
    EventBus::instance().clear();

    // 最后恢复终端（幂等，析构时会再次调用）
    terminal.restore();

    return 0;
}

} // namespace agent

int main(int argc, char* argv[]) {
    return agent::run(argc, argv);
}
