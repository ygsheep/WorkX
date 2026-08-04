/**
 * @file main.cpp
 * @brief Workx TUI 入口
 * @details 解析参数，组装各层，运行
 * @version 4.0.0
 * @date 2026-07
 *
 * v4.0.0 变更（D-2/D-3）：
 *   - 依赖组装逻辑提取到 app/factory.h（init_logger / make_terminal_config /
 *     create_session / register_builtin_tools / build_system_prompt）
 *   - IBackend 拆分为 ICompletionProvider + IBackendAdmin（D-3 接口隔离）
 *   - main.cpp 仅保留 TUI 接线与事件订阅
 */

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include <liblogger/logger.h>

#include "agent/api/chat_types.h"
#include "agent/api/i_backend.h"
#include "agent/api/remote/http_client.h"
#include "agent/command/inclaude/registry.h"
#include "agent/input/processor.h"
#include "agent/core/chat_session.h"
#include "agent/message/types.h"
#include "agent/model/provider_preset.h"
#include "agent/model/context_resolver.h"
#include "agent/model/model_catalog.h"
#include "agent/session/session_store.h"  // 项目会话恢复：get_project_session_dir
#include "agent/skill/inclaude/skill_prompt.h"
#include "agent/tool/SkillTool/skill_tool.h"
#include "agent/tool/registry.h"
#include "app/command/builtin_commands.h"
#include "app/command/skill_commands.h"
#include "app/config/app_config.h"
#include "app/config/cli_args.h"
#include "app/factory.h"
#include "app/ui/model_selector.h"
#include "app/ui/path_completer.h"
#include "app/ui/file_index.h"
#include "app/ui/provider_form.h"
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
#include "tui/widgets/session_picker.h"  // 启动恢复 + /resume 会话选择面板

namespace agent {

// L-4：移除 `using namespace tui;`（namespace agent 内的命名空间污染）。
// 改为显式 `tui::` 前缀引用 TUI 层类型（Terminal / Screen / SetupWizard /
// ChatRenderer / BottomBarMode / ColorRole / CommandEntry 等）。

// ============================================================
// 主函数
// ============================================================

static int run(int argc, char* argv[]) {
    // ---- 注册配置元数据 ----
    // M-2：显式注入 ConfigManager 单例（main.cpp 作为装配层负责依赖组装）
    auto& cfg_manager = ConfigManager::instance();
    register_config_defaults(cfg_manager);

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
        load_from_config_file(cfg_manager, explicit_config_path);
    } else {
        // 尝试默认配置文件
        auto default_path = default_config_path();
        if (std::filesystem::exists(default_path)) {
            load_from_config_file(cfg_manager, default_path);
        }
    }

    // 2. 环境变量（中等优先级）
    load_from_env(cfg_manager);

    // 3. CLI 参数（最高优先级）
    parse_cli_args(cfg_manager, argc, argv);

    // ---- 从 ConfigManager 读取配置 ----
    auto& cfg = cfg_manager;  // M-2：复用装配层已获取的单例引用

    // ---- 初始化日志系统（D-2：委托工厂）----
    init_logger(cfg, default_log_path());

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

    // ---- Terminal（D-2：委托工厂构建 config；H-4：显式注入三大依赖）----
    tui::Terminal terminal(&EventBus::instance(),
                      &ConfigManager::instance(),
                      &TaskManager::instance(),
                      make_terminal_config(cfg));

    auto init_result = terminal.initialize();
    if (init_result.isErr()) {
        std::cerr << "Failed to initialize terminal: " << init_result.error() << "\n";
        terminal.restore();
        return 1;
    }

    // ---- 文件索引构建（TUI 启动时扫描工作目录）----
    // issue #15-D: 从用户主目录启动时跳过 FileIndex 扫描，避免扫描海量文件导致卡顿
    {
        namespace fs = std::filesystem;
        std::string cwd = fs::current_path().string();

        // 检测 cwd 是否为用户主目录（Windows: USERPROFILE/APPDATA，POSIX: HOME）
        bool is_home_dir = false;
        const char* home_envs[] = {
#ifdef _WIN32
            "USERPROFILE", "APPDATA"
#else
            "HOME"
#endif
        };
        for (const char* env : home_envs) {
            if (const char* home = std::getenv(env)) {
                std::error_code ec;
                if (fs::equivalent(cwd, home, ec)) {
                    is_home_dir = true;
                    break;
                }
            }
        }

        auto& index = global_file_index();
        if (is_home_dir) {
            std::cerr << "[warn] Running from home directory; skipping file index. "
                         "Use a project directory for full indexing.\n";
        } else {
            index.build(cwd);
            if (verbose) {
                std::cerr << "[debug] File index built: " << index.size() << " files\n";
            }
        }
    }

    // ---- 首次运行向导 ----
    // 当 provider、api_key、remote 都没配置时，启动交互式设置向导
    bool needs_wizard = cfg.get_or<std::string>(keys::PROVIDER, "").empty()
                     && cfg.get_or<std::string>(keys::API_KEY, "").empty()
                     && cfg.get_or<std::string>(keys::REMOTE_URL, "").empty();
    // ---- 差分渲染引擎（供向导等交互式 UI 使用） ----
    tui::Screen screen(&terminal);

    if (needs_wizard) {
        // H-A：SetupWizard 接收 IConfigManager&（面板需 get/has/set 读写 backend.* 键
        //       与 save_to_file 持久化，接口在 core/config 层）；
        //       面板函数与配置路径由 app 层注入（回调），tui 层不依赖 app 层。
        tui::SetupWizard wizard(
            terminal.platform(), &terminal, &screen, cfg, default_config_path(),
            &provider_manager_interactive);
        bool ok = wizard.run_wizard();
        if (!ok) {
            terminal.restore();
            return 0;
        }
        // Screen 内部已通过 clear() + flush() 清空虚拟屏幕
    }

    // ---- 检查 Provider Preset ----
    std::string provider_name = cfg.get_or<std::string>(keys::PROVIDER, "");
    const ProviderPreset* preset = provider_name.empty() ? nullptr : find_preset(provider_name);

    // ---- 上下文窗口镜像回填 ----
    // backend.context_length 标量是"使用中供应商"的镜像，providers 数组才是源。
    // 旧版切换写入 0 或用户手改配置导致标量缺失时，从数组回填当前使用中条目的显式配置，
    // 保证 statusbar resolver 与压缩器水位（factory 读同一标量）一致。
    if (cfg.get_or<int>(keys::CONTEXT_LENGTH, 0) <= 0) {
        std::string active_id = cfg.get_or<std::string>(keys::PROVIDER, "");
        if (!active_id.empty()) {
            for (const auto& e : load_provider_configs(cfg)) {
                if (e.id == active_id && e.context_length > 0) {
                    cfg.set(keys::CONTEXT_LENGTH, static_cast<int>(e.context_length));
                    break;
                }
            }
        }
    }

    // ---- models.dev 远程目录（ModelCatalog）----
    // 启动时先加载本地缓存（快、离线可用）→ 后台线程拉取远程 → 成功写缓存 + 更新内存。
    // 拉取失败保留现有缓存，不影响启动。
    // 缓存路径：<config_dir>/models_cache.json
    constexpr const char* kModelsDevUrl = "https://models.dev/api.json";
    // 堆上持有原子指针：后台 detach 线程按值捕获，退出/切换时无栈悬垂；
    // 后台 store() 替换与前台 load() 并发安全（atomic），值传递给 resolver 贯穿调用期
    auto model_catalog = std::make_shared<std::atomic<std::shared_ptr<const ModelCatalog>>>();
    auto catalog_cache_path = default_config_path().parent_path() / "models_cache.json";
    if (auto cached = ModelCatalog::load_cache(catalog_cache_path); cached.is_ok()) {
        model_catalog->store(std::make_shared<const ModelCatalog>(std::move(cached.value())));
    }
    // 后台线程拉取（不阻塞启动）
    std::thread catalog_refresh_thread([model_catalog, catalog_cache_path]() {
        // 缓存新鲜度：24h 内已拉取过则跳过（启动即离线命中，避免每次启动都 30s 网络窗口）
        constexpr auto kCacheTtl = std::chrono::hours(24);
        std::error_code ec;
        auto mtime = std::filesystem::last_write_time(catalog_cache_path, ec);
        if (!ec && std::filesystem::file_time_type::clock::now() - mtime < kCacheTtl) {
            return;
        }
        HttpClient http;
        auto resp = http.get(kModelsDevUrl, {}, /*timeout_ms=*/30000);
        if (resp.is_err() || !resp.value().is_success()) return;
        auto parsed = ModelCatalog::from_api_json(resp.value().body);
        if (parsed.is_err()) return;
        // 先写缓存再更新内存（保证下次启动也能离线命中）
        parsed.value().save_cache(catalog_cache_path);
        model_catalog->store(std::make_shared<const ModelCatalog>(std::move(parsed.value())));
    });
    catalog_refresh_thread.detach();

    // ---- Backend + Session（D-2：委托工厂）----
    auto session_result = create_session(cfg, preset,
                                         TaskManager::instance(),
                                         EventBus::instance());
    auto session = std::move(session_result.session);
    std::string remote_url = std::move(session_result.remote_url);
    std::string model_name = std::move(session_result.model_name);
    // H-8：UI 层通过 IBackendAdmin* 调用 list_models / set_model_name，
    // 不再依赖 ChatSession::backend() 暴露完整 IBackend*。
    // 生命周期：session 持有 backend，session 存活期间 admin 有效。
    auto backend_admin = session_result.backend_admin;

    // 项目会话恢复：检测历史会话，有则直接打开 TUI 选择面板（统一 UX，无乱码）
    // 注意：选择面板在 renderer 创建前弹出（overlay 不依赖 renderer），
    //       实际的 switch_session + replay_history 延迟到 renderer 就绪后执行。
    std::string pending_restore_path;
    if (session) {
        namespace fs = std::filesystem;
        fs::path config_dir = default_config_path().parent_path();
        std::string cwd = fs::current_path().string();
        fs::path project_dir = agent::session::get_project_session_dir(config_dir, cwd);

        // 检查是否有历史会话
        auto sessions = agent::session::SessionStore::list_sessions(project_dir.string());
        if (!sessions.empty()) {
            // 有历史会话，打开 SessionPicker 面板
            pending_restore_path = pick_session_interactive(
                &terminal, &screen, project_dir.string());
        }
    }

    if (session && verbose) {
        std::cerr << "[debug] Backend ready\n";
        std::cerr << "[debug]   provider: " << (preset ? preset->name : "openai") << "\n";
        std::cerr << "[debug]   url:      " << remote_url << "\n";
        std::cerr << "[debug]   model:    " << model_name << "\n";
    }

    std::shared_ptr<command::CommandRegistry> registry;
    std::unique_ptr<agent::input::InputProcessor> input_processor;

    // ---- 命令系统 ----
    registry = std::make_shared<command::CommandRegistry>();
    // H-C：显式构造 LocalFileLoader，避免依赖 InputProcessor 构造函数默认实参
    input_processor = std::make_unique<agent::input::InputProcessor>(
        registry, std::make_shared<agent::input::LocalFileLoader>());

    // ---- 启动时模型选择（model_name 为空时触发） ----
    // H-8：使用 factory 注入的 backend_admin 替代 session->backend()
    if (session && backend_admin && model_name.empty()) {
        ModelSelection sel = select_model_interactive(
            cfg, &terminal, &screen, backend_admin, model_name);
        if (!sel.name.empty()) {
            cfg.set(keys::MODEL_NAME, sel.name);
            backend_admin->set_model_name(sel.name);
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
    tui::ChatRenderer renderer(&terminal);
    renderer.start();

    // 启动时恢复历史会话：renderer 就绪后执行 switch_session + replay_history
    // show_welcome=true：启动恢复时先渲染欢迎横幅再渲染历史消息
    if (session && !pending_restore_path.empty()) {
        if (session->switch_session(pending_restore_path)) {
            renderer.replay_history(session->get_messages(), /*show_welcome=*/true);
        }
    }

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
        // issue #15-D: cwd 为用户主目录时显示 "（无项目）" 而非目录名（如 "young"）
        namespace fs = std::filesystem;
        std::string cwd = fs::current_path().string();
        bool is_home_dir = false;
        const char* home_envs[] = {
#ifdef _WIN32
            "USERPROFILE", "APPDATA"
#else
            "HOME"
#endif
        };
        for (const char* env : home_envs) {
            if (const char* home = std::getenv(env)) {
                std::error_code ec;
                if (fs::equivalent(cwd, home, ec)) {
                    is_home_dir = true;
                    break;
                }
            }
        }
        sb->set_project_name(is_home_dir ? "\xEF\xBC\x88\xE6\x97\xA0\xE9\xA1\xB9\xE7\x9B\xAE\xEF\xBC\x89"
                                          : fs::current_path().filename().string());

        // 上下文窗口：统一通过 resolver 解析（优先级：provider→user cfg→catalog→capability→preset→default）
        // 启动初始化时无 selector 返回值，sel_context_length 传 0；
        // 若启动时已通过 select_model_interactive 选择模型并持久化了 context_length，
        // 这里读 cfg 即可拿到（来源为 ProviderList 的值已 save_to_file）
        auto resolution = resolve_context_length(
            model_name,
            /*sel_context_length=*/0,
            cfg.get_or<int>(keys::CONTEXT_LENGTH, 0),
            preset,
            model_catalog->load());
        sb->set_context_limit(resolution.value);
    }

    // 注册内置系统命令（help/exit/quit/clear/regen/model）
    command::SystemCommandContext sys_ctx;
    // 间接引用：指向本函数持有的 session 变量。热切换（/provider）替换 session
    // 后 clear/regen/rename 命令自动跟随新对象，避免拷贝裸指针悬垂（use-after-free）。
    sys_ctx.session = &session;
    sys_ctx.on_exit = []() {
        EventBus::instance().publish(ShutdownEvent{.force = false});
    };
    sys_ctx.on_model_select = [&terminal, &screen, &backend_admin, &cfg, &renderer, &preset, model_catalog]() {
        // H-8：使用 factory 注入的 backend_admin 替代 session->backend()
        if (!backend_admin) {
            terminal.set_color(tui::ColorRole::Error);
            terminal.write("No backend configured. Use --provider first.\n");
            terminal.reset_color();
            return;
        }
        ModelSelection sel = select_model_interactive(
            cfg, &terminal, &screen, backend_admin,
            cfg.get_or<std::string>(keys::MODEL_NAME, ""));
        if (!sel.name.empty()) {
            cfg.set(keys::MODEL_NAME, sel.name);
            backend_admin->set_model_name(sel.name);
            if (auto* sb = renderer.status_bar()) {
                sb->set_model_name(sel.name);
                // 上下文窗口：统一通过 resolver 解析（优先级：provider→user cfg→catalog→capability→preset→default）
                auto resolution = resolve_context_length(
                    sel.name,
                    sel.context_length,
                    cfg.get_or<int>(keys::CONTEXT_LENGTH, 0),
                    preset,
                    model_catalog->load());
                sb->set_context_limit(resolution.value);
                // 仅当来源是 ProviderList 时持久化（避免用兜底值覆盖用户配置）
                if (resolution.source == ContextLengthResolution::Source::ProviderList) {
                    cfg.set(keys::CONTEXT_LENGTH, static_cast<int>(resolution.value));
                }
            }
            cfg.save_to_file(default_config_path());
            terminal.set_color(tui::ColorRole::System);
            terminal.write(std::format("Model set to: {}\n", sel.name));
            terminal.reset_color();
        }
    };
    sys_ctx.on_provider_select = [&terminal, &screen, &cfg, &session, &backend_admin,
                                  &renderer, &preset, model_catalog]() {
        ProviderSwitchResult sel = provider_manager_interactive(cfg, &terminal, &screen);
        if (!sel.applied) {
            return;  // 用户取消或仅编辑配置列表
        }
        // 持久化多供应商列表 + 使用中标量键
        cfg.save_to_file(default_config_path());

        // ---- 热切换：重建 session，保留当前对话继续 ----
        if (!session) {
            terminal.set_color(tui::ColorRole::System);
            terminal.write(std::format(
                "Provider set to: {} (model: {}). Restart to apply.\n",
                sel.entry.name, sel.entry.model.empty() ? "(custom)" : sel.entry.model));
            terminal.reset_color();
            return;
        }

        // 生成中禁止热切换：替换 session 会触发旧 ChatSession 析构
        // （~ChatSession 执行 interrupt + wait，最长 30s 阻塞 UI 线程），
        // 且切换瞬间导入的消息列表不一致。提示用户先中断当前回复。
        if (session->is_generating()) {
            terminal.set_color(tui::ColorRole::Error);
            terminal.write("正在生成中，请先按 Ctrl+C 中断当前回复，再切换 Provider\n");
            terminal.reset_color();
            return;
        }

        // 1. 备份当前消息与 SessionStore（继续写同一会话文件）
        std::vector<ChatMessage> messages = session->get_messages();
        auto store = session->session_store();

        // 2. 用新 provider 重建 session
        const ProviderPreset* new_preset = find_preset(sel.entry.id);
        auto new_result = create_session(
            cfg, new_preset, TaskManager::instance(), EventBus::instance());
        if (!new_result.session) {
            terminal.set_color(tui::ColorRole::Error);
            terminal.write("Provider 切换失败：无法创建后端（检查 URL 与网络）\n");
            terminal.reset_color();
            return;
        }

        // 3. 导入当前对话消息 + 延续 SessionStore（会话文件不中断）
        new_result.session->import_messages(std::move(messages));
        if (store) new_result.session->set_session_store(store);

        // 4. 替换全局引用
        session = std::move(new_result.session);
        backend_admin = new_result.backend_admin;
        preset = new_preset;

        // 5. 状态栏 + 重放历史（保留当前对话继续）
        if (auto* sb = renderer.status_bar()) {
            sb->set_model_name(new_result.model_name.empty() ? "unknown" : new_result.model_name);
            auto resolution = resolve_context_length(
                new_result.model_name,
                /*sel_context_length=*/0,
                cfg.get_or<int>(keys::CONTEXT_LENGTH, 0),
                new_preset,
                model_catalog->load());
            sb->set_context_limit(resolution.value);
        }
        renderer.replay_history(session->get_messages());
        terminal.set_color(tui::ColorRole::System);
        terminal.write(std::format(
            "已切换到 Provider: {} (model: {})\n",
            sel.entry.name, new_result.model_name.empty() ? "(custom)" : new_result.model_name));
        terminal.reset_color();
    };
    sys_ctx.on_resume = [&session, &terminal, &screen, &renderer]() {
        if (!session) return;

        // 获取项目会话目录
        namespace fs = std::filesystem;
        fs::path config_dir = default_config_path().parent_path();
        std::string cwd = fs::current_path().string();
        fs::path project_dir = agent::session::get_project_session_dir(config_dir, cwd);

        // 打开会话选择面板（空列表也显示，面板内提示"没有可恢复会话"）
        std::string selected_path = pick_session_interactive(&terminal, &screen, project_dir.string());

        if (selected_path.empty()) {
            // 用户取消（Esc/Ctrl+C）或无历史会话
            return;
        }

        // 切换会话
        if (session->switch_session(selected_path)) {
            // 重绘历史消息到输出区域
            renderer.replay_history(session->get_messages());
            terminal.set_color(tui::ColorRole::System);
            terminal.write("已切换到历史会话\n");
            terminal.reset_color();
        } else {
            terminal.set_color(tui::ColorRole::Error);
            terminal.write("切换会话失败\n");
            terminal.reset_color();
        }
    };
    command::register_system_commands(*registry, sys_ctx);

    // 加载磁盘 skills（.claude/skills）并注册为命令
    {
        namespace fs = std::filesystem;
        const auto cwd = fs::current_path().string();
        command::register_skill_commands(*registry, cwd);
        // SkillTool 在 factory 中以空 registry 注册，此处注入命令注册表
        if (session) {
            const auto tool_registry = session->tool_registry();
            if (tool_registry) {
                if (auto* skill_tool = dynamic_cast<tool::SkillTool*>(
                        tool_registry->find_by_name("Skill").get())) {
                    skill_tool->set_registry(registry);
                }
            }
            // 注入 skills 列表到 system prompt（factory 构建时 registry 尚不存在）
            const auto skills_section = skill::build_skills_prompt_section(*registry);
            if (!skills_section.empty()) {
                session->set_system_prompt(session->system_prompt() + skills_section);
            }
        }
    }

    // CommandPanel 初始化（从 CommandRegistry 获取命令列表）
    // registry 由 make_shared 创建，保证非空，无需空检查
    {
        std::vector<tui::CommandEntry> entries;
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
        if (bottom_bar.mode() != tui::BottomBarMode::STATUS_BAR) return;
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
            // is_local_command=true：本地命令输出不累加 token 统计
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
                    .was_interrupted = false,
                    .is_local_command = true
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
                    session->send_message(query_text, std::move(result.image_paths));
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
    // 顺序：unsubscribe → cancelAll → waitForAll → renderer.stop → reset session → clear EventBus → restore
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

    // issue #15-C: 显式 reset session，确保 ~ChatSession / ~backend 在 EventBus
    // 仍可用时析构，避免 clear() 后 on_complete 回调访问已失效订阅导致 abort
    // 注意：backend_admin 是裸指针，由 session 持有，session.reset() 后不可再使用

    // 项目会话恢复：关闭 SessionStore 文件（懒创建可能未创建，需动态获取）
    // 不写 session_end：会话可被多次 resume 继续，session_end 会破坏语义
    if (session) {
        auto store = session->session_store();
        if (store) {
            store->close();
        }
    }

    session.reset();

    // 清空 EventBus 订阅，防止后续异步事件触发已失效的回调
    EventBus::instance().clear();

    // 最后恢复终端（幂等，析构时会再次调用）
    terminal.restore();

    return 0;
}

} // namespace agent

int main(int argc, char* argv[]) {
    // issue #15-E: Debug 构建注册 std::set_terminate 提供未捕获异常诊断
    // 避免 abort() 弹窗无任何上下文信息，便于定位线程异常逃逸根因
#ifndef NDEBUG
    std::set_terminate([]() noexcept {
        try {
            std::cerr << "\n[FATAL] std::terminate called. ";
            if (auto ptr = std::current_exception()) {
                std::rethrow_exception(ptr);
            } else {
                std::cerr << "No active exception.\n";
            }
        } catch (const std::exception& e) {
            std::cerr << "Uncaught exception: " << e.what() << "\n";
        } catch (...) {
            std::cerr << "Uncaught unknown exception.\n";
        }
        std::cerr.flush();
        std::abort();
    });
#endif
    return agent::run(argc, argv);
}
