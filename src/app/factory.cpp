/**
 * @file factory.cpp
 * @brief 应用层依赖组装工厂实现（D-2）
 * @details 从 main.cpp 提取的可测试组装逻辑
 * @version 1.0.0
 * @date 2026-07
 */

#include <algorithm>
#include <format>
#include <string>

#include <liblogger/logger.h>

#include "app/config/app_config.h"
#include "app/factory.h"
#include "agent/api/backend_factory.h"
#include "agent/api/chat_types.h"
#include "agent/api/i_backend.h"
#include "agent/core/chat_session.h"
#include "agent/model/provider_preset.h"
#include "agent/tool/FileEditTool/file_edit_tool.h"
#include "agent/tool/FileReadTool/file_read_tool.h"
#include "agent/tool/FileWriteTool/file_write_tool.h"
#include "agent/tool/registry.h"
#include "core/config/config_manager.h"
#include "core/events/event_bus.h"
#include "core/task/task_manager.h"
#include "tui/core/terminal.h"

namespace agent {

// ============================================================
// init_logger
// ============================================================

void init_logger(IConfigManager& cfg, const std::filesystem::path& default_log_path) {
    auto& logger = agent::log::Logger::get_instance();

    // 日志级别
    std::string level_str = cfg.get_or<std::string>(keys::LOG_LEVEL, "info");
    std::transform(level_str.begin(), level_str.end(), level_str.begin(), ::tolower);
    if (level_str == "trace")       logger.set_level(agent::log::LogLevel::TRACE);
    else if (level_str == "debug")  logger.set_level(agent::log::LogLevel::DEBUG);
    else if (level_str == "warn")   logger.set_level(agent::log::LogLevel::WARN);
    else if (level_str == "error")  logger.set_level(agent::log::LogLevel::ERROR);
    else if (level_str == "fatal")  logger.set_level(agent::log::LogLevel::FATAL);
    else                            logger.set_level(agent::log::LogLevel::INFO);

    // 日志文件
    std::string log_file = cfg.get_or<std::string>(keys::LOG_FILE, "");
    if (log_file.empty()) {
        log_file = default_log_path.string();
    }
    logger.enable_file_output(log_file, true);
}

// ============================================================
// make_terminal_config
// ============================================================

tui::TerminalConfig make_terminal_config(IConfigManager& cfg) {
    tui::TerminalConfig config;
    config.simple_io = cfg.get_or<bool>(keys::SIMPLE_IO, false);
    config.use_color = !cfg.get_or<bool>(keys::NO_COLOR, false);
    config.prompt_string = cfg.get_or<std::string>(keys::PROMPT, "> ");
    return config;
}

// ============================================================
// create_session
// ============================================================

SessionResult create_session(IConfigManager& cfg, const ProviderPreset* preset) {
    SessionResult result;

    // URL: cfg(显式设置) > preset > ""
    if (cfg.has(keys::REMOTE_URL)) {
        result.remote_url = cfg.get_or<std::string>(keys::REMOTE_URL, "");
    } else if (preset && !preset->default_url.empty()) {
        result.remote_url = std::string(preset->default_url);
    }

    // Model: cfg(显式设置) > preset > ""
    if (cfg.has(keys::MODEL_NAME)) {
        result.model_name = cfg.get_or<std::string>(keys::MODEL_NAME, "");
    } else if (preset && !preset->default_model.empty()) {
        result.model_name = std::string(preset->default_model);
    }

    // 无 remote_url 时不创建 backend
    if (result.remote_url.empty()) {
        return result;
    }

    // 构建 BackendConfig
    BackendConfig backend_config;
    backend_config.type = BackendConfig::Type::Remote;
    backend_config.provider = preset ? preset->type : ProviderType::OpenAI;
    backend_config.base_url = result.remote_url;
    backend_config.model_name = result.model_name;
    backend_config.api_key = cfg.get_or<std::string>(keys::API_KEY, "");
    int default_timeout = preset && preset->timeout_ms > 0 ? preset->timeout_ms : 30000;
    backend_config.timeout_ms = cfg.get_or<int>(keys::TIMEOUT_MS, default_timeout);

    // 创建后端
    auto backend = BackendFactory::create(backend_config);
    if (!backend) {
        return result;  // session 保持 nullptr
    }

    // 初始化后端（V2-3：initialize 返回 ResultV2）
    auto init_result = backend->initialize(backend_config);
    if (init_result.is_err()) {
        return result;  // session 保持 nullptr
    }

    // 构造 ChatSession（DI 三件套）
    int default_retry_delay = preset && preset->retry_delay_ms > 0 ? preset->retry_delay_ms : 1000;
    result.session = std::make_unique<ChatSession>(
        std::move(backend), default_retry_delay, "default",
        TaskManager::instance(),
        EventBus::instance(),
        ConfigManager::instance());

    // 注册内置工具
    auto tool_registry = std::make_shared<tool::ToolRegistry>();
    register_builtin_tools(*tool_registry);
    result.session->set_tool_registry(tool_registry);

    // 系统提示词
    std::string sys_prompt = build_system_prompt(
        cfg.get_or<std::string>(keys::SYSTEM_PROMPT, ""), *tool_registry);
    if (!sys_prompt.empty()) {
        result.session->set_system_prompt(sys_prompt);
    }

    return result;
}

// ============================================================
// register_builtin_tools
// ============================================================

void register_builtin_tools(tool::ToolRegistry& registry) {
    registry.register_tool(std::make_shared<tool::FileReadTool>());
    registry.register_tool(std::make_shared<tool::FileWriteTool>());
    registry.register_tool(std::make_shared<tool::FileEditTool>());
}

// ============================================================
// build_system_prompt
// ============================================================

std::string build_system_prompt(const std::string& user_prompt,
                                const tool::ToolRegistry& registry) {
    std::string sys_prompt = user_prompt;

    // 拼接工具 prompt
    for (const auto& t : registry.get_all_tools()) {
        sys_prompt += "\n\n";
        sys_prompt += t->prompt();
    }

    // @file 引用说明
    sys_prompt +=
        "\n\n"
        "用户消息中可能出现 <file path=\"...\">...</file> 标签，这是用户通过 "
        "@path 语法引用的文件内容，已由前端读取并注入。对此类标签内的路径，"
        "禁止再次调用 Read 工具读取；直接基于标签内已有内容回答用户问题。";

    return sys_prompt;
}

} // namespace agent
