/**
 * @file factory.h
 * @brief 应用层依赖组装工厂（D-2）— B1 后为宿主薄封装
 * @details 宿主无关的会话装配已上提到 workx_agent（agent/factory.h）：
 *          - create_session / register_builtin_tools / build_system_prompt
 *          本头文件仅保留依赖 tui 的函数，并 re-export agent/factory.h
 *          保持调用方（app/main.cpp、test_factory.cpp）不感知迁移。
 * @version 1.1.0
 * @date 2026-08
 */

#pragma once

#include <filesystem>

#include "core/config/i_config_manager.h"
#include "agent/factory.h"  // re-export：SessionResult / create_session / register_builtin_tools / build_system_prompt

namespace tui { struct TerminalConfig; }

namespace agent {

/// @brief 初始化日志系统
/// @details 从配置读取 level 和 file 路径，配置 Logger
/// @param cfg 配置管理器
/// @param default_log_path 默认日志文件路径（cfg 中 LOG_FILE 为空时使用）
void init_logger(IConfigManager& cfg, const std::filesystem::path& default_log_path);

/// @brief 初始化审计日志系统（#37）
/// @details 独立于运行日志的结构化 JSONL 审计日志，记录工具调用和安全事件。
///          默认路径为 ~/.workx/logs/audit/audit.jsonl。
/// @param cfg 配置管理器
void init_audit_logger(IConfigManager& cfg);

/// @brief 从配置构建 TerminalConfig（tui 专属，保留在宿主层）
/// @param cfg 配置管理器
/// @return 填充好的 TerminalConfig
tui::TerminalConfig make_terminal_config(IConfigManager& cfg);

} // namespace agent
