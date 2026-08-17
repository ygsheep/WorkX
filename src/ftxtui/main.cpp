/**
 * @file main.cpp
 * @brief codex 入口 — FTXUI 双栏实验 TUI
 * @details 复刻 create_session 的核心装配（不链接 workx_app，避免拖动 workx_tui）。
 *          复用 workx_agent + workx_core。见 docs/plans/2026-08-17-ftxui-tui-design.md。
 * @version 0.1.0（实验）
 */

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <liblogger/logger.h>

#include "agent/api/backend_factory.h"
#include "agent/api/i_backend.h"
#include "agent/api/i_backend_admin.h"
#include "agent/command/inclaude/registry.h"
#include "agent/config/app_config.h"
#include "agent/core/chat_session.h"
#include "agent/input/i_file_loader.h"
#include "agent/input/processor.h"
#include "agent/model/provider_preset.h"
#include "agent/session/session_store.h"
#include "agent/tool/AskUser/AskUserTool.h"
#include "agent/tool/BashTool/bash_tool.h"
#include "agent/tool/FileEditTool/file_edit_tool.h"
#include "agent/tool/FileReadTool/file_read_tool.h"
#include "agent/tool/FileWriteTool/file_write_tool.h"
#include "agent/tool/GlobTool/glob_tool.h"
#include "agent/tool/GrepTool/grep_tool.h"
#include "agent/tool/PowerShellTool/powershell_tool.h"
#include "agent/tool/registry.h"
#include "core/config/config_manager.h"
#include "core/events/event_bus.h"
#include "core/events/stream_events.h"
#include "core/task/task_manager.h"

#include "app.h"

namespace ftxtui {

// ---------------------------------------------------------------------------
// 会话装配（workx_app::create_session 的轻量复刻）
// ---------------------------------------------------------------------------

static void register_min_tools(agent::tool::ToolRegistry& registry) {
    registry.register_tool(std::make_shared<agent::tool::FileReadTool>());
    registry.register_tool(std::make_shared<agent::tool::FileWriteTool>());
    registry.register_tool(std::make_shared<agent::tool::FileEditTool>());
    registry.register_tool(std::make_shared<agent::tool::BashTool>());
    registry.register_tool(std::make_shared<agent::tool::GlobTool>());
    registry.register_tool(std::make_shared<agent::tool::GrepTool>());
    registry.register_tool(std::make_shared<agent::tool::AskUserTool>());
#ifdef _WIN32
    registry.register_tool(std::make_shared<agent::tool::PowerShellTool>());
#endif
}

static std::string build_sys_prompt() {
    namespace fs = std::filesystem;
    std::string p;
    p += "# Environment\n";
    p += "- Working directory: " + fs::current_path().string() + "\n";
#ifdef _WIN32
    p += "- Platform: win32\n";
#else
    p += "- Platform: unix\n";
#endif
    p += "\n用户消息中可能出现 <file path=\"...\">...</file> 标签，这是用户通过 @path "
         "语法引用的文件内容，已由前端读取并注入。对此类标签内的路径，禁止再次调用 "
         "Read 工具读取；直接基于标签内已有内容回答。";
    return p;
}

static std::unique_ptr<agent::ChatSession> create_min_session(
    agent::ConfigManager& cfg,
    agent::ITaskManager& task_manager,
    agent::IEventBus& event_bus,
    std::string& model_name_out,
    std::string& project_dir_out) {
    (void)event_bus;

    std::string provider = cfg.get_or<std::string>(agent::keys::PROVIDER, "");
    std::string remote_url = cfg.get_or<std::string>(agent::keys::REMOTE_URL, "");
    std::string api_key = cfg.get_or<std::string>(agent::keys::API_KEY, "");
    std::string model = cfg.get_or<std::string>(agent::keys::MODEL_NAME, "");

    const agent::ProviderPreset* preset = provider.empty() ? nullptr
                                                           : agent::find_preset(provider);
    if (remote_url.empty() && preset && !preset->default_url.empty())
        remote_url = preset->default_url;
    if (model.empty() && preset && !preset->default_model.empty())
        model = preset->default_model;
    if (remote_url.empty()) return nullptr;

    agent::BackendConfig backend_config;
    backend_config.type = agent::BackendConfig::Type::Remote;
    backend_config.provider = preset ? preset->type : agent::ProviderType::OpenAI;
    backend_config.base_url = remote_url;
    backend_config.model_name = model;
    backend_config.api_key = api_key;
    int default_timeout = (preset && preset->timeout_ms > 0) ? preset->timeout_ms : 30000;
    backend_config.timeout_ms = cfg.get_or<int>(agent::keys::TIMEOUT_MS, default_timeout);

    auto backend = agent::BackendFactory::create(backend_config, &event_bus);
    if (!backend) return nullptr;
    auto init_result = backend->initialize(backend_config);
    if (init_result.is_err()) return nullptr;

    int retry = (preset && preset->retry_delay_ms > 0) ? preset->retry_delay_ms : 1000;
    auto session = std::make_unique<agent::ChatSession>(
        std::move(backend), task_manager, event_bus, cfg, retry, "ftx");

    if (cfg.get_or<bool>(agent::keys::BYPASS_PERMISSIONS, false)) {
        session->set_permission_mode(agent::tool::PermissionMode::BypassPermissions);
    }

    auto registry = std::make_shared<agent::tool::ToolRegistry>();
    register_min_tools(*registry);
    session->set_tool_registry(registry);

    std::string sys = build_sys_prompt();
    if (!sys.empty()) session->set_system_prompt(sys);

    int32_t ctx = cfg.get_or<int>(agent::keys::CONTEXT_LENGTH, 0);
    if (ctx > 0) session->set_compactor_context_window(ctx);

    // 会话持久化（首条 user 消息时懒创建 SessionStore，JSONL 实时追加）
    namespace fs = std::filesystem;
    auto config_dir = agent::default_config_path().parent_path();
    project_dir_out = agent::session::get_project_session_dir(
        config_dir, fs::current_path().string()).string();
    session->configure_session_store(project_dir_out, fs::current_path().string(), model, "");

    model_name_out = model;
    return session;
}

}  // namespace ftxtui

/// @brief 订阅用户输入事件 → InputProcessor 管线（复刻 src/app/main.cpp）
/// @details UserInputEvent 经 InputParser(解析层) → InputProcessor(处理层) →
///          根据 ProcessResult 调用执行层（本地命令输出 / send_message）。
static void setup_input_pipeline(agent::IEventBus& bus,
                                 std::unique_ptr<agent::ChatSession>& session,
                                 std::shared_ptr<agent::command::CommandRegistry> registry) {
    auto input_processor = std::make_unique<agent::input::InputProcessor>(
        registry, std::make_shared<agent::input::LocalFileLoader>());

    bus.subscribe<agent::UserInputEvent>(
        [&session, &input_processor, &bus](const agent::UserInputEvent& e) {
            if (!input_processor) return;

            agent::command::CommandContext ctx;
            auto result = input_processor->process(e.text, ctx);

            if (result.is_error) {
                bus.publish(agent::StreamErrorEvent{
                    .session_id = "default",
                    .message = result.output_text,
                    .retryable = false,
                });
                return;
            }

            // 本地命令有输出文本 → 直接发布（is_local_command=true：不累加 token 统计）
            if (!result.output_text.empty() && !result.should_query) {
                bus.publish(agent::StreamTokenEvent{
                    .session_id = "default",
                    .content_delta = result.output_text,
                    .reasoning_delta = "",
                    .is_thinking = false,
                    .token_count = 0,
                });
                bus.publish(agent::StreamDoneEvent{
                    .session_id = "default",
                    .full_content = result.output_text,
                    .full_reasoning = "",
                    .was_interrupted = false,
                    .is_local_command = true,
                });
                return;
            }

            // 需要调 LLM
            if (!result.should_query) return;
            if (session) {
                std::string query_text;
                if (!result.output_text.empty()) {
                    query_text = result.output_text;
                } else if (!result.messages.empty()) {
                    for (size_t i = 0; i < result.messages.size(); ++i) {
                        if (i > 0) query_text += "\n\n";
                        query_text += result.messages[i];
                    }
                } else if (!e.text.empty() && e.text[0] != '/') {
                    query_text = e.text;
                } else {
                    bus.publish(agent::StreamErrorEvent{
                        .session_id = "default",
                        .message = "命令展开为空，已取消发送",
                        .retryable = false,
                    });
                    return;
                }
                session->send_message(query_text, std::move(result.image_paths));
            } else {
                // 无后端时回显
                bus.publish(agent::StreamTokenEvent{
                    .session_id = "default",
                    .content_delta = "Echo: " + e.text + "\n",
                    .reasoning_delta = "",
                    .is_thinking = false,
                    .token_count = 0,
                });
                bus.publish(agent::StreamDoneEvent{
                    .session_id = "default",
                    .full_content = "Echo: " + e.text + "\n",
                    .full_reasoning = "",
                    .was_interrupted = false,
                });
            }
        });
}

int main(int argc, char** argv) {
    bool mock_mode = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--mock") mock_mode = true;
    }
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
    auto command_registry = std::make_shared<agent::command::CommandRegistry>();
    if (!mock_mode) {
        session = ftxtui::create_min_session(cfg, tm, bus, model_name, session_dir);
        setup_input_pipeline(bus, session, command_registry);
    }

    ftxtui::AppDeps deps;
    deps.session = session.get();
    deps.event_bus = &bus;
    deps.mock_mode = mock_mode;
    deps.model_name = model_name;
    deps.session_dir = session_dir;
    deps.command_registry = command_registry;
    deps.project = fs::current_path().filename().string();
    deps.agent_name = "default";
    deps.on_submit = [&](const std::string& text) {
        if (session) session->send_message(text);
    };

    ftxtui::App app(std::move(deps));
    app.run();

    // 清理
    tm.cancelAll();
    tm.waitForAll();
    if (session) {
        auto store = session->session_store();
        if (store) store->close();
    }
    session.reset();
    bus.clear();
    return 0;
}