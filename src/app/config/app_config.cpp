/**
 * @file app_config.cpp
 * @brief 应用配置加载实现
 * @details 配置 Schema 注册、环境变量加载、配置文件加载、默认路径
 * @version 2.0.0
 * @date 2026-07
 *
 * v2.0.0 变更（C-2/C-4）：
 *   - register_meta → register_schema（结构化类型/范围/枚举约束）
 *   - 6 个标准环境变量绑定到 Schema，由 ConfigManager::load_from_env() 统一加载
 *   - WORKX_NO_COLOR 保留 presence-only 语义（兼容 NO_COLOR 规范），手动处理
 */

#include <cstdlib>
#include <iostream>

#include "app/config/app_config.h"
#include "core/config/config_manager.h"
#include "core/config/i_config_manager.h"
#include "agent/tool/constants.h"

namespace agent {

void register_config_defaults(ConfigManager& cfg) {

    // === Terminal ===
    cfg.register_schema({
        .key = keys::SIMPLE_IO,
        .description = "Use simple I/O mode (getline)",
        .default_value = false,
        .type = ConfigSchema::Type::Bool
    });
    cfg.register_schema({
        .key = keys::NO_COLOR,
        .description = "Disable colored output (env WORKX_NO_COLOR: presence-only)",
        .default_value = false,
        .type = ConfigSchema::Type::Bool
        // env_var 留空：presence-only 语义需手动处理，见 load_from_env()
    });
    cfg.register_schema({
        .key = keys::VERBOSE,
        .description = "Show verbose startup debug info",
        .default_value = false,
        .type = ConfigSchema::Type::Bool
    });
    cfg.register_schema({
        .key = keys::PROMPT,
        .description = "Prompt string",
        .default_value = std::string("> "),
        .type = ConfigSchema::Type::String
    });

    // === Backend ===
    cfg.register_schema({
        .key = keys::REMOTE_URL,
        .description = "Remote API base URL (OpenAI-compatible)",
        .default_value = std::string(""),
        .is_required = false,
        .type = ConfigSchema::Type::String,
        .env_var = "WORKX_BASE_URL"
    });
    cfg.register_schema({
        .key = keys::MODEL_NAME,
        .description = "Model name for remote API",
        .default_value = std::string(""),
        .type = ConfigSchema::Type::String,
        .env_var = "WORKX_MODEL"
    });
    cfg.register_schema({
        .key = keys::API_KEY,
        .description = "API key for remote API",
        .default_value = std::string(""),
        .type = ConfigSchema::Type::String,
        .env_var = "WORKX_API_KEY"
    });
    cfg.register_schema({
        .key = keys::PROVIDER,
        .description = "Provider name (openai, anthropic, deepseek, groq, together, openai-compatible)",
        .default_value = std::string(""),
        .type = ConfigSchema::Type::Enum,
        .enum_values = {"", "openai", "anthropic", "deepseek", "groq", "together", "openai-compatible"}
    });
    cfg.register_schema({
        .key = keys::TIMEOUT_MS,
        .description = "HTTP timeout in milliseconds",
        .default_value = 30000,
        .type = ConfigSchema::Type::Int,
        .int_range = std::make_pair<int64_t, int64_t>(1, 86400000),  // 1ms ~ 24h
        .env_var = "WORKX_TIMEOUT"
    });
    cfg.register_schema({
        .key = keys::CONTEXT_LENGTH,
        .description = "Model context window (tokens), overrides preset default",
        .default_value = 0,  // 0 表示未设置，由 provider preset 决定
        .type = ConfigSchema::Type::Int,
        .int_range = std::make_pair<int64_t, int64_t>(0, 2000000)
    });

    // === Retry ===
    cfg.register_schema({
        .key = keys::RETRY_COUNT,
        .description = "Max retry attempts for transient errors",
        .default_value = 3,
        .type = ConfigSchema::Type::Int,
        .int_range = std::make_pair<int64_t, int64_t>(0, 100)
    });
    cfg.register_schema({
        .key = keys::RETRY_DELAY_MS,
        .description = "Initial retry delay in ms (doubles each retry)",
        .default_value = 1000,
        .type = ConfigSchema::Type::Int,
        .int_range = std::make_pair<int64_t, int64_t>(1, 3600000)  // 1ms ~ 1h
    });

    // === Session ===
    cfg.register_schema({
        .key = keys::SYSTEM_PROMPT,
        .description = "System prompt for chat session",
        .default_value = std::string(""),
        .type = ConfigSchema::Type::String
    });
    cfg.register_schema({
        .key = keys::SAVE_PATH,
        .description = "Default save path for /save command",
        .default_value = std::string(""),
        .type = ConfigSchema::Type::String
    });

    // === Logging ===
    cfg.register_schema({
        .key = keys::LOG_LEVEL,
        .description = "Log level",
        .default_value = std::string("info"),
        .type = ConfigSchema::Type::Enum,
        .enum_values = {"trace", "debug", "info", "warn", "error", "fatal"},
        .env_var = "WORKX_LOG_LEVEL"
    });
    cfg.register_schema({
        .key = keys::LOG_FILE,
        .description = "Log file path (empty to disable file logging)",
        .default_value = std::string(""),
        .type = ConfigSchema::Type::String,
        .env_var = "WORKX_LOG_FILE"
    });

    // === Tool — FileReadTool ===
    cfg.register_schema({
        .key = keys::FILE_READ_MAX_SIZE,
        .description = "Max file size in bytes for Read tool (default 2MB)",
        .default_value = static_cast<int>(agent::tool::constants::MAX_FILE_SIZE_BYTES),
        .type = ConfigSchema::Type::Int,
        .int_range = std::make_pair<int64_t, int64_t>(1, 1024 * 1024 * 1024)  // 1B ~ 1GB
    });
    cfg.register_schema({
        .key = keys::FILE_READ_MAX_LINES,
        .description = "Max lines to read per call for Read tool (default 2000)",
        .default_value = agent::tool::constants::MAX_LINES_TO_READ,
        .type = ConfigSchema::Type::Int,
        .int_range = std::make_pair<int64_t, int64_t>(1, 1000000)
    });

    // === Tool — FileEditTool ===
    cfg.register_schema({
        .key = keys::EDIT_DENY_PATTERNS,
        .description = "Newline-separated glob patterns for paths denied by Edit tool "
                       "(e.g. \"~/.ssh/**\\n**/.env\\n**/.git/**\")",
        .default_value = std::string(""),
        .type = ConfigSchema::Type::String
    });
    cfg.register_schema({
        .key = keys::EDIT_SCAN_SECRETS,
        .description = "Scan new_string for potential secrets before editing (default false)",
        .default_value = false,
        .type = ConfigSchema::Type::Bool
    });
}

void load_from_env(ConfigManager& cfg) {

    // 1. 由 ConfigManager 统一加载已绑定到 Schema 的环境变量
    //    覆盖：WORKX_API_KEY / WORKX_BASE_URL / WORKX_MODEL / WORKX_TIMEOUT
    //          / WORKX_LOG_LEVEL / WORKX_LOG_FILE
    cfg.load_from_env();

    // 2. WORKX_NO_COLOR 采用 presence-only 语义（兼容 https://no-color.org 规范）：
    //    环境变量存在且非空即启用 no_color，不依赖值解析
    if (const char* val = std::getenv("WORKX_NO_COLOR")) {
        if (val[0] != '\0') {
            cfg.set(keys::NO_COLOR, true);
        }
    }
}

void load_from_config_file(IConfigManager& cfg, const std::filesystem::path& path) {
    auto result = cfg.load_from_file(path);
    if (result.is_err()) {
        std::cerr << "Warning: " << result.error().to_string() << "\n";
    }
}

// F.5：统一配置目录解析，优先级链：
//   1. $WORKX_CONFIG_DIR 环境变量（用户/管理员显式指定）
//   2. $APPDATA (Windows) / $XDG_CONFIG_HOME (POSIX)
//   3. $USERPROFILE (Windows) / $HOME (POSIX)
//   4. 当前工作目录（最后回退，避免从快捷方式启动时配置丢失）
static std::filesystem::path get_config_dir() {
    // 1. 显式环境变量优先
    if (const char* env = std::getenv("WORKX_CONFIG_DIR")) {
        if (env[0] != '\0') return std::filesystem::path(env);
    }
#ifdef _WIN32
    if (const char* env = std::getenv("APPDATA")) {
        if (env[0] != '\0') return std::filesystem::path(env) / "workx";
    }
    if (const char* env = std::getenv("USERPROFILE")) {
        if (env[0] != '\0') return std::filesystem::path(env) / ".workx";
    }
#else
    if (const char* env = std::getenv("XDG_CONFIG_HOME")) {
        if (env[0] != '\0') return std::filesystem::path(env) / "workx";
    }
    if (const char* env = std::getenv("HOME")) {
        if (env[0] != '\0') return std::filesystem::path(env) / ".config" / "workx";
    }
#endif
    // 4. 最后回退：当前工作目录下的 .workx
    return std::filesystem::current_path() / ".workx";
}

std::filesystem::path default_config_path() {
    return get_config_dir() / "config.json";
}

std::filesystem::path default_log_path() {
    return get_config_dir() / "logs" / "workx.log";
}

} // namespace agent
