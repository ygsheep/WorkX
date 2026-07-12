/**
 * @file model_selector.h
 * @brief 交互式模型选择
 * @details 启动时和 /model 命令共用的模型选择 UI
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>

namespace agent {

class Terminal;
class Screen;
class IBackend;

/// @brief 交互式模型选择（启动时和 /model 命令共用）
/// @param term 终端
/// @param scr 屏幕
/// @param bk 后端（用于获取模型列表）
/// @param current_model 当前模型名
/// @return 选中的模型名，空字符串表示取消
std::string select_model_interactive(
    Terminal* term, Screen* scr, IBackend* bk,
    const std::string& current_model);

} // namespace workx
