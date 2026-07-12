/**
 * @file app_config.h
 * @brief 应用配置键定义与配置加载
 * @details 配置键常量、默认值注册、环境变量/配置文件加载
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <filesystem>

namespace agent {

/// 配置键字符串常量
namespace keys {
    // Terminal
    constexpr const char* SIMPLE_IO    = "terminal.simple_io";
    constexpr const char* NO_COLOR     = "terminal.no_color";
    constexpr const char* VERBOSE      = "terminal.verbose";
    constexpr const char* PROMPT       = "terminal.prompt";

    // Backend
    constexpr const char* REMOTE_URL   = "backend.remote_url";
    constexpr const char* MODEL_NAME   = "backend.model_name";
    constexpr const char* API_KEY      = "backend.api_key";
    constexpr const char* PROVIDER     = "backend.provider";
    constexpr const char* TIMEOUT_MS   = "backend.timeout_ms";

    // Retry
    constexpr const char* RETRY_COUNT    = "backend.retry_count";
    constexpr const char* RETRY_DELAY_MS = "backend.retry_delay_ms";

    // Session
    constexpr const char* SYSTEM_PROMPT  = "session.system_prompt";
    constexpr const char* SAVE_PATH      = "session.save_path";

    // Logging
    constexpr const char* LOG_LEVEL      = "logging.level";
    constexpr const char* LOG_FILE       = "logging.file";
}

/// @brief 注册所有配置项的元数据（描述、默认值、验证函数）
void register_config_defaults();

/// @brief 从环境变量加载配置（WORKX_API_KEY, WORKX_BASE_URL 等）
void load_from_env();

/// @brief 从配置文件加载
void load_from_config_file(const std::filesystem::path& path);

/// @brief 默认配置文件路径（平台相关）
std::filesystem::path default_config_path();

/// @brief 默认日志文件路径（平台相关）
std::filesystem::path default_log_path();

} // namespace workx
