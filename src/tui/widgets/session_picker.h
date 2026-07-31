/**
 * @file session_picker.h
 * @brief 会话选择面板（/resume 命令使用）
 * @details 全屏 overlay 交互式选择历史会话：
 *          - 顶部搜索框（实时过滤）
 *          - 下方会话列表（标题 + 时间 + 分支 + 消息数）
 *          - 上下键选择，Enter 确认，Esc 取消
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <optional>
#include <vector>

#include "agent/session/session_store.h"

namespace tui { class Terminal; class Screen; }

namespace agent {

/// @brief 交互式会话选择（/resume 命令调用）
/// @param term 终端
/// @param scr 屏幕
/// @param project_dir 项目会话目录（<config_dir>/projects/<编码路径>）
/// @return 选中的会话文件路径；空字符串表示取消
std::string pick_session_interactive(
    tui::Terminal* term, tui::Screen* scr,
    const std::string& project_dir);

} // namespace agent
