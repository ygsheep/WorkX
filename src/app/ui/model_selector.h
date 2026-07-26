/**
 * @file model_selector.h
 * @brief 交互式模型选择
 * @details 启动时和 /model 命令共用的模型选择 UI
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <cstdint>
#include <string>

namespace agent {

class Terminal;
class Screen;
class IBackend;

/// @brief 模型选择结果
/// @details select_model_interactive 返回，携带模型名与上下文窗口大小
struct ModelSelection {
    std::string name;               ///< 选中的模型名，空表示取消
    int32_t context_length = 0;     ///< 模型上下文窗口（token），0 表示未知
};

/// @brief 交互式模型选择（启动时和 /model 命令共用）
/// @param term 终端
/// @param scr 屏幕
/// @param bk 后端（用于获取模型列表）
/// @param current_model 当前模型名
/// @return 选中的模型及上下文窗口；name 为空表示取消
ModelSelection select_model_interactive(
    Terminal* term, Screen* scr, IBackend* bk,
    const std::string& current_model);

} // namespace agent
