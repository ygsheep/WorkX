/**
 * @file main.cpp
 * @brief codex 入口 — FTXUI 双栏实验 TUI
 * @details 复用 workx_agent 的会话装配（agent::create_session，B1 统一）：
 *          与 workx 主程序共享同一工厂（Backend + 全量工具集 + 系统提示词 +
 *          会话持久化），不再各自维护 create_min_session 造成工具集漂移。
 *          不链接 workx_app（避免拖动 workx_tui）。见
 *          docs/plans/2026-08-17-ftxui-tui-design.md。
 * @version 0.1.0（实验）
 */

#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <liblogger/logger.h>

#include "agent/api/i_backend_admin.h"
#include "agent/api/remote/http_client.h"  // models.dev 目录后台刷新
#include "agent/command/inclaude/registry.h"
#include "agent/config/app_config.h"
#include "agent/core/chat_session.h"
#include "agent/factory.h"
#include "agent/model/context_resolver.h"  // 上下文窗口解析（侧栏进度条分母）
#include "agent/model/model_catalog.h"
#include "agent/model/provider_preset.h"
#include "agent/session/session_store.h"
#include "core/config/config_manager.h"
#include "core/events/event_bus.h"
#include "core/task/task_manager.h"
#include "core/utils/file_index.h"

#include "app.h"
#include "crash_reporter.h"

int main(int argc, char** argv) {
    crash::InstallHandlers();
    bool mock_mode = false;
    bool smoke_mode = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--mock") mock_mode = true;
        if (std::string(argv[i]) == "--smoke") smoke_mode = true;
    }
    // 冒烟（B5）依赖 mock 流：无后端也能在 CI 无头管道下跑通全链路
    if (smoke_mode) mock_mode = true;
    namespace fs = std::filesystem;
    auto& cfg = agent::ConfigManager::instance();
    agent::register_config_defaults(cfg);

    // 载入默认配置文件（存在则读）
    auto config_path = agent::default_config_path();
    if (fs::exists(config_path)) agent::load_from_config_file(cfg, config_path);
    agent::load_from_env(cfg);

    // 日志（Debug/Release 统一到 ~/.workx/logs）
    agent::log::Logger::get_instance().set_level(agent::log::LogLevel::LOG_INFO);
    auto log_file = agent::default_log_path();
    if (!log_file.empty()) {
        agent::log::Logger::get_instance().enable_file_output(log_file.string(), true);
    }

    auto& bus = agent::EventBus::instance();
    auto& tm = agent::TaskManager::instance();

    std::string model_name;
    std::string session_dir;
    std::unique_ptr<agent::ChatSession> session;
    agent::IBackendAdmin* backend_admin = nullptr;
    auto command_registry = std::make_shared<agent::command::CommandRegistry>();
    // 上下文窗口（token）：启动时经 resolve_context_length 解析，注入侧栏进度条分母
    int32_t context_limit = 0;
    // models.dev 目录：堆上原子指针，后台 detach 线程按值捕获，前台 load() 并发安全
    auto model_catalog = std::make_shared<std::atomic<std::shared_ptr<const agent::ModelCatalog>>>();

    if (!mock_mode) {
        // B1：与 workx 主程序共用同一会话装配（全量工具集 + 系统提示词 + 持久化）
        const std::string provider = cfg.get_or<std::string>(agent::keys::PROVIDER, "");
        const agent::ProviderPreset* preset = provider.empty() ? nullptr
                                                               : agent::find_preset(provider);
        auto result = agent::create_session(cfg, preset, tm, bus);
        session = std::move(result.session);
        model_name = result.model_name;
        backend_admin = result.backend_admin;

        // 会话持久化目录（/resume 列出历史用）
        if (session) {
            auto config_dir = agent::default_config_path().parent_path();
            session_dir = agent::session::get_project_session_dir(
                config_dir, fs::current_path().string()).string();
        }

        // 上下文窗口：统一通过 resolver 解析（provider→user cfg→catalog→capability→preset→default）
        // 启动初始化时无 selector 返回值，sel_context_length 传 0；对齐 src/app/main.cpp
        auto catalog_cache_path = agent::default_config_path().parent_path() / "models_cache.json";
        if (auto cached = agent::ModelCatalog::load_cache(catalog_cache_path); cached.is_ok()) {
            model_catalog->store(std::make_shared<const agent::ModelCatalog>(std::move(cached.value())));
        }
        // 后台线程拉取（不阻塞启动）；24h 内已拉取过则跳过（离线命中）
        std::thread catalog_refresh_thread([model_catalog, catalog_cache_path]() {
            constexpr auto kCacheTtl = std::chrono::hours(24);
            std::error_code ec;
            auto mtime = std::filesystem::last_write_time(catalog_cache_path, ec);
            if (!ec && std::filesystem::file_time_type::clock::now() - mtime < kCacheTtl) return;
            agent::HttpClient http;
            auto resp = http.get("https://models.dev/api.json", {}, /*timeout_ms=*/30000);
            if (resp.is_err() || !resp.value().is_success()) return;
            auto parsed = agent::ModelCatalog::from_api_json(resp.value().body);
            if (parsed.is_err()) return;
            parsed.value().save_cache(catalog_cache_path);
            model_catalog->store(std::make_shared<const agent::ModelCatalog>(std::move(parsed.value())));
        });
        catalog_refresh_thread.detach();

        auto resolution = agent::resolve_context_length(
            model_name,
            /*sel_context_length=*/0,
            cfg.get_or<int>(agent::keys::CONTEXT_LENGTH, 0),
            preset,
            model_catalog->load());
        context_limit = resolution.value;
    }

    // ---- 文件索引异步构建（@ 补全面板数据源）----
    // 后台线程扫描工作目录，不阻塞 TUI 出现；索引未就绪时 @ 面板返回空，
    // 就绪后自动显示文件列表。从用户主目录启动时跳过，避免扫描海量文件卡顿。
    {
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
            if (const char* home = getenv(env)) {
                std::error_code ec;
                if (fs::equivalent(cwd, home, ec)) {
                    is_home_dir = true;
                    break;
                }
            }
        }
        if (!is_home_dir) {
            agent::global_file_index().build_async(cwd);  // 后台线程构建，不阻塞启动
        }
    }

    ftxtui::AppDeps deps;
    deps.session = session.get();
    deps.backend_admin = backend_admin;
    deps.event_bus = &bus;
    deps.config_manager = &cfg;
    deps.task_manager = &tm;
    deps.mock_mode = mock_mode;
    deps.smoke_mode = smoke_mode;
    deps.model_name = model_name;
    deps.session_dir = session_dir;
    deps.context_limit = context_limit;
    deps.model_catalog = model_catalog;
    deps.command_registry = command_registry;
    deps.project = fs::current_path().filename().string();
    // B3：侧栏 Agent 显示真实会话 ID（非硬编码 "default"），
    //     与审计日志 / 事件流的 session_id 一致，便于对照
    deps.agent_name = (session && !session->session_id().empty())
                          ? session->session_id()
                          : "default";
    deps.on_submit = [&](const std::string& text) {
        if (session) session->send_message(text);
    };
    // /provider 热切换：复用统一后端工厂（预设名或自定义条目 id 均可解析）
    deps.create_provider = [&cfg, &bus](const std::string& name) {
        return agent::create_backend(cfg, agent::find_preset(name), bus);
    };

    ftxtui::App app(std::move(deps));
    app.run();
    const int exit_code = app.exit_code();

    // 清理
    tm.cancelAll();
    tm.waitForAll();
    if (session) {
        auto store = session->session_store();
        if (store) store->close();
    }
    session.reset();
    bus.clear();
    return exit_code;
}