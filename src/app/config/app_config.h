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

class IConfigManager;
class ConfigManager;

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
    constexpr const char* CONTEXT_LENGTH = "backend.context_length";  // 模型上下文窗口（token），覆盖 preset 默认值

    // Retry
    constexpr const char* RETRY_COUNT    = "backend.retry_count";
    constexpr const char* RETRY_DELAY_MS = "backend.retry_delay_ms";

    // Session
    constexpr const char* SYSTEM_PROMPT  = "session.system_prompt";
    constexpr const char* SAVE_PATH      = "session.save_path";

    // Logging
    constexpr const char* LOG_LEVEL      = "logging.level";
    constexpr const char* LOG_FILE       = "logging.file";

    // Tool — FileReadTool
    constexpr const char* FILE_READ_MAX_SIZE  = "tool.file_read.max_file_size_bytes";
    constexpr const char* FILE_READ_MAX_LINES = "tool.file_read.max_lines_to_read";

    // Tool — FileEditTool
    /// 拒绝编辑的路径模式列表（换行分隔的 glob，如 "~/.ssh/**\n**/.env\n**/.git/**"）
    constexpr const char* EDIT_DENY_PATTERNS  = "tool.edit.deny_patterns";
    /// 是否启用密钥扫描（写入内容包含疑似密钥时拒绝）
    constexpr const char* EDIT_SCAN_SECRETS   = "tool.edit.scan_secrets";
}

/// @brief 注册所有配置项的结构化 Schema（类型/默认值/范围/枚举/环境变量映射）
/// @details v2.0.0 起使用 ConfigSchema 替代 ConfigMeta，由 ConfigManager 在
///          set_value() 时自动校验。环境变量绑定通过 Schema.env_var 声明。
/// @note M-2：接收 ConfigManager&（非 IConfigManager&），因 register_schema 是
///             ConfigManager 特有方法，不属于 IConfigManager 接口。配置初始化
///             本质是引导阶段，与业务组件的可测试性需求分离。
void register_config_defaults(ConfigManager& cfg);

/// @brief 从环境变量加载配置
/// @details 6 个标准环境变量由 ConfigManager::load_from_env() 按 Schema 加载：
///          WORKX_API_KEY/BASE_URL/MODEL/TIMEOUT/LOG_LEVEL/LOG_FILE。
///          WORKX_NO_COLOR 采用 presence-only 语义（兼容 no-color.org 规范）。
/// @note M-2：接收 ConfigManager&，原因同 register_config_defaults。
void load_from_env(ConfigManager& cfg);

/// @brief 从配置文件加载
/// @note M-2：接收 IConfigManager&，可注入 Mock 测试配置加载逻辑。
void load_from_config_file(IConfigManager& cfg, const std::filesystem::path& path);

/// @brief 默认配置文件路径（平台相关）
std::filesystem::path default_config_path();

/// @brief 默认日志文件路径（平台相关）
std::filesystem::path default_log_path();

} // namespace agent
