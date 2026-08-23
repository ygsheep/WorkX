/**
 * @file status_line.h
 * @brief composer 上方单行状态行（模型 / 权限 / 忙碌）
 */

#pragma once

#include <string>

#include <ftxui/dom/elements.hpp>

#include "vm/view_model.h"

namespace ftxtui {

/// @brief 构建状态行元素（内嵌于输入区下方，单行）
/// @param model 模型名
/// @param permission 权限标签（"" / "plan" / "bypass"）
/// @param busy 是否生成中
/// @param todo_done 已完成待办数（#24：>0 时显示 ✓ X/Y）
/// @param todo_total 待办总数
ftxui::Element build_status_line(const std::string& model,
                                 const std::string& permission,
                                 bool busy,
                                 int todo_done = 0,
                                 int todo_total = 0);

}  // namespace ftxtui