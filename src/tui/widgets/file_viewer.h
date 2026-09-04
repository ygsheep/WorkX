/**
 * @file file_viewer.h
 * @brief 文件查看视图（/view 只读）：路径栏 + 行号 + 虚拟化滚动 + 状态栏
 * @details 纯渲染组件：状态由调用方（App）持有，本组件只负责渲染。
 *          文件行高固定 1，scroll 偏移 + 可视行数切片实现虚拟化滚动。
 */

#pragma once

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

#include "vm/view_model.h"

namespace ftxtui {

/// @brief 构建文件查看 tab 组件（可聚焦：↑↓/PgUp/PgDn/滚轮滚动）
/// @param file 文件 tab 状态（App 持有；path 为空 = 未打开，显示占位）
ftxui::Component make_file_viewer(FileViewState* file);

/// @brief 构建文件查看视图
/// @param file 文件 tab 状态（path 为空 = 未打开，显示占位）
/// @param avail_width 内容可用宽度（≤0 = 不折行；组件的 TB 调用侧传渲染宽度）
/// @param avail_height 可视行数（>0 时绕过终端探测，用于无头测试/组件传实渲染高度；≤0 走终端高度）
ftxui::Element build_file_viewer(const FileViewState& file, int avail_width = 0,
                                 int avail_height = 0);

}  // namespace ftxtui
