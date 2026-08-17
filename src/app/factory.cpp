/**
 * @file factory.cpp
 * @brief 应用层依赖组装工厂实现（D-2）— B1 后为宿主薄封装
 * @details 宿主无关的会话装配已上提到 workx_agent（agent/factory.cpp）。
 *          本文件仅保留依赖 tui 的函数：init_logger / init_audit_logger /
 *          make_terminal_config。
 * @version 1.1.0
 * @date 2026-08
 */

#include <algorithm>
#include <filesystem>
#include <string>

#include <liblogger/logger.h>

#include "agent/audit/audit_logger.h"
#include "agent/config/app_config.h"
#include "app/factory.h"
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
    if (level_str == "trace")       logger.set_level(agent::log::LogLevel::LOG_TRACE);
    else if (level_str == "debug")  logger.set_level(agent::log::LogLevel::LOG_DEBUG);
    else if (level_str == "warn")   logger.set_level(agent::log::LogLevel::LOG_WARN);
    else if (level_str == "error")  logger.set_level(agent::log::LogLevel::LOG_ERROR);
    else if (level_str == "fatal")  logger.set_level(agent::log::LogLevel::LOG_FATAL);
    else                            logger.set_level(agent::log::LogLevel::LOG_INFO);

    // 日志文件
    std::string log_file = cfg.get_or<std::string>(keys::LOG_FILE, "");
    if (log_file.empty()) {
        log_file = default_log_path.string();
    }
    logger.enable_file_output(log_file, true);
}

// ============================================================
// init_audit_logger
// ============================================================

void init_audit_logger(IConfigManager& cfg) {
    // 审计日志路径：默认 ~/.workx/logs/audit/audit.jsonl
    auto config_dir = agent::default_config_path().parent_path();

    // 配置键：audit.enabled / audit.file / audit.max_size_mb / audit.retention_days
    bool enabled = cfg.get_or<bool>(agent::keys::AUDIT_ENABLED, true);
    if (!enabled) {
        audit::AuditLogger::instance().set_enabled(false);
        return;
    }

    std::string audit_file = cfg.get_or<std::string>(agent::keys::AUDIT_FILE, "");
    if (audit_file.empty()) {
        audit_file = (config_dir / "logs" / "audit" / "audit.jsonl").string();
    }

    size_t max_size_mb = static_cast<size_t>(cfg.get_or<int>(agent::keys::AUDIT_MAX_SIZE_MB, 10));
    size_t retention_days = static_cast<size_t>(cfg.get_or<int>(agent::keys::AUDIT_RETENTION_DAYS, 30));

    audit::AuditLogger::instance().init(audit_file, max_size_mb, retention_days);
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

} // namespace agent
