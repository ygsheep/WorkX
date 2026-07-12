/**
 * @file types.h
 * @brief 命令相关类型定义
 * @details Availability, LoadSource, CommandContext, CommandResult, PromptBlock 等类型
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <vector>
#include <optional>
#include <nlohmann/json.hpp>

namespace agent::command {

/// 命令可用性 — 对应 CommandAvailability
enum class Availability {
    Universal,    ///< 无限制，默认
    ClaudeAI,     ///< 仅 claude.ai 订阅用户
    Console,      ///< 仅 Console API key 用户
};

/// 命令加载来源 — 对应 loadedFrom
enum class LoadSource {
    Builtin,
    Skills,
    Plugin,
    Bundled,
    MCP,
    Deprecated,
};

/// 命令执行上下文
/// 对应精简版 LocalJSXCommandContext
struct CommandContext {
    std::string cwd;                 ///< 工作目录
    std::string model;               ///< 当前模型名称
    std::string session_id;          ///< 会话 ID
    nlohmann::json options;          ///< 额外选项
};

/// 命令结果 — 对应 LocalCommandResult
struct CommandResult {
    enum class Type {
        Text,       ///< 文本输出
        Compact,    ///< 压缩结果
        Skip,       ///< 跳过消息
    };

    Type type{Type::Text};
    std::string text;              ///< 文本输出内容
    nlohmann::json compact_data;   ///< 压缩结果数据（可选）
    bool is_error{false};          ///< 是否错误

    static CommandResult ok(std::string text) {
        return {.type = Type::Text, .text = std::move(text)};
    }

    static CommandResult error(std::string msg) {
        return {.type = Type::Text, .text = std::move(msg), .is_error = true};
    }
};

/// 提示词内容块 — 对应 ContentBlockParam
struct PromptBlock {
    std::string type;           ///< "text" | "image" | "tool_result"
    std::string text;           ///< 文本内容
    nlohmann::json image;       ///< 图片内容（可选）
};

} // namespace agent::command
