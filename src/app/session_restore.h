/**
 * @file session_restore.h
 * @brief 项目会话恢复：启动时询问用户是否恢复历史会话
 * @details 在 terminal 初始化前用简单终端 I/O 列出历史会话，
 *          避免与 raw 模式冲突。用户选择恢复则返回文件路径，
 *          选择开新会话则返回 nullopt。
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <optional>
#include <string>

namespace agent {

/// @brief 启动时检查并询问用户是否恢复上次会话
/// @param project_dir 项目会话目录路径（<config_dir>/projects/<编码路径>）
/// @return 选中的会话文件路径（用户选择开新会话或无历史时返回 nullopt）
/// @details 在 terminal 初始化前调用，使用 std::cin/cout 交互。
///          最多展示最近 5 条会话，按修改时间倒序排列。
std::optional<std::string> prompt_restore_session(const std::string& project_dir);

} // namespace agent
