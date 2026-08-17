/**
 * @file sidebar.h
 * @brief 右侧上下文/成本面板（opencode 双栏侧栏，窄屏自动折叠）
 */

#pragma once

#include <ftxui/dom/elements.hpp>

#include "vm/view_model.h"

namespace ftxtui {

/// @brief 构建侧栏元素（标题 / 上下文进度 / token / 成本 / 权限 / agent）
ftxui::Element build_sidebar(const SidebarModel& s);

/// @brief 构建点状分隔行
ftxui::Element sidebar_rule();

}  // namespace ftxtui