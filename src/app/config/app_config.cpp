/**
 * @file app_config.cpp
 * @brief 应用配置加载实现
 * @details 配置元数据注册、环境变量加载、配置文件加载、默认路径
 * @version 1.0.0
 * @date 2026-07
 */

#include <cstdlib>
#include <iostream>

#include "app/config/app_config.h"
#include "core/config/config_manager.h"
#include "agent/tool/constants.h"

namespace agent {

void register_config_defaults() {
    auto& cfg = ConfigManager::instance();

    cfg.register_meta(keys::SIMPLE_IO, {
        .description = "Use simple I/O mode (getline)",
        .default_value = false
    });
    cfg.register_meta(keys::NO_COLOR, {
        .description = "Disable colored output",
        .default_value = false
    });
    cfg.register_meta(keys::VERBOSE, {
        .description = "Show verbose startup debug info",
        .default_value = false
    });
    cfg.register_meta(keys::PROMPT, {
        .description = "Prompt string",
        .default_value = std::string("> ")
    });
    cfg.register_meta(keys::REMOTE_URL, {
        .description = "Remote API base URL (OpenAI-compatible)",
        .default_value = std::string(""),
        .is_required = false
    });
    cfg.register_meta(keys::MODEL_NAME, {
        .description = "Model name for remote API",
        .default_value = std::string("")
    });
    cfg.register_meta(keys::API_KEY, {
        .description = "API key for remote API",
        .default_value = std::string("")
    });
    cfg.register_meta(keys::PROVIDER, {
        .description = "Provider name (openai, anthropic, deepseek, groq, together, openai-compatible)",
        .default_value = std::string("")
    });
    cfg.register_meta(keys::TIMEOUT_MS, {
        .description = "HTTP timeout in milliseconds",
        .default_value = 30000,
        .validate_callback = [](const ConfigValue& v) -> Result<void, std::string> {
            if (std::holds_alternative<int>(v) && std::get<int>(v) <= 0) {
                return Result<void, std::string>::err("timeout must be positive");
            }
            return Result<void, std::string>::ok();
        }
    });
    cfg.register_meta(keys::RETRY_COUNT, {
        .description = "Max retry attempts for transient errors",
        .default_value = 3,
        .validate_callback = [](const ConfigValue& v) -> Result<void, std::string> {
            if (std::holds_alternative<int>(v) && std::get<int>(v) < 0) {
                return Result<void, std::string>::err("retry_count must be >= 0");
            }
            return Result<void, std::string>::ok();
        }
    });
    cfg.register_meta(keys::RETRY_DELAY_MS, {
        .description = "Initial retry delay in ms (doubles each retry)",
        .default_value = 1000,
        .validate_callback = [](const ConfigValue& v) -> Result<void, std::string> {
            if (std::holds_alternative<int>(v) && std::get<int>(v) <= 0) {
                return Result<void, std::string>::err("retry_delay_ms must be positive");
            }
            return Result<void, std::string>::ok();
        }
    });
    cfg.register_meta(keys::SYSTEM_PROMPT, {
        .description = "System prompt for chat session",
        .default_value = std::string("")
    });
    cfg.register_meta(keys::SAVE_PATH, {
        .description = "Default save path for /save command",
        .default_value = std::string("")
    });
    cfg.register_meta(keys::LOG_LEVEL, {
        .description = "Log level (trace/debug/info/warn/error/fatal)",
        .default_value = std::string("info")
    });
    cfg.register_meta(keys::LOG_FILE, {
        .description = "Log file path (empty to disable file logging)",
        .default_value = std::string("")
    });

    // Tool — FileReadTool
    cfg.register_meta(keys::FILE_READ_MAX_SIZE, {
        .description = "Max file size in bytes for Read tool (default 2MB)",
        .default_value = static_cast<int>(agent::tool::constants::MAX_FILE_SIZE_BYTES),
        .validate_callback = [](const ConfigValue& v) -> Result<void, std::string> {
            if (std::holds_alternative<int>(v) && std::get<int>(v) <= 0) {
                return Result<void, std::string>::err("max_file_size_bytes must be positive");
            }
            return Result<void, std::string>::ok();
        }
    });
    cfg.register_meta(keys::FILE_READ_MAX_LINES, {
        .description = "Max lines to read per call for Read tool (default 2000)",
        .default_value = agent::tool::constants::MAX_LINES_TO_READ,
        .validate_callback = [](const ConfigValue& v) -> Result<void, std::string> {
            if (std::holds_alternative<int>(v) && std::get<int>(v) <= 0) {
                return Result<void, std::string>::err("max_lines_to_read must be positive");
            }
            return Result<void, std::string>::ok();
        }
    });

    // Tool — FileEditTool
    cfg.register_meta(keys::EDIT_DENY_PATTERNS, {
        .description = "Newline-separated glob patterns for paths denied by Edit tool "
                       "(e.g. \"~/.ssh/**\\n**/.env\\n**/.git/**\")",
        .default_value = std::string("")
    });
    cfg.register_meta(keys::EDIT_SCAN_SECRETS, {
        .description = "Scan new_string for potential secrets before editing (default false)",
        .default_value = false
    });
}

void load_from_env() {
    auto& cfg = ConfigManager::instance();

    if (const char* val = std::getenv("WORKX_API_KEY")) {
        cfg.set(keys::API_KEY, std::string(val));
    }
    if (const char* val = std::getenv("WORKX_BASE_URL")) {
        cfg.set(keys::REMOTE_URL, std::string(val));
    }
    if (const char* val = std::getenv("WORKX_MODEL")) {
        cfg.set(keys::MODEL_NAME, std::string(val));
    }
    if (const char* val = std::getenv("WORKX_TIMEOUT")) {
        try {
            cfg.set(keys::TIMEOUT_MS, std::stoi(val));
        } catch (...) {}
    }
    if (std::getenv("WORKX_NO_COLOR") != nullptr) {
        cfg.set(keys::NO_COLOR, true);
    }
    if (const char* val = std::getenv("WORKX_LOG_LEVEL")) {
        cfg.set(keys::LOG_LEVEL, std::string(val));
    }
    if (const char* val = std::getenv("WORKX_LOG_FILE")) {
        cfg.set(keys::LOG_FILE, std::string(val));
    }
}

void load_from_config_file(const std::filesystem::path& path) {
    auto result = ConfigManager::instance().load_from_file(path);
    if (result.isErr()) {
        std::cerr << "Warning: " << result.error() << "\n";
    }
}

std::filesystem::path default_config_path() {
#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    if (appdata) {
        return std::filesystem::path(appdata) / "workx" / "config.json";
    }
    return std::filesystem::path("workx.json");
#else
    const char* home = std::getenv("HOME");
    if (home) {
        return std::filesystem::path(home) / ".config" / "workx" / "config.json";
    }
    return std::filesystem::path("workx.json");
#endif
}

std::filesystem::path default_log_path() {
#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    if (appdata) {
        return std::filesystem::path(appdata) / "workx" / "logs" / "workx.log";
    }
    return std::filesystem::path("workx.log");
#else
    const char* home = std::getenv("HOME");
    if (home) {
        return std::filesystem::path(home) / ".config" / "workx" / "logs" / "workx.log";
    }
    return std::filesystem::path("workx.log");
#endif
}

} // namespace workx
