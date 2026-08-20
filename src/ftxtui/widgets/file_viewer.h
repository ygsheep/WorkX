/**
 * @file file_viewer.h
 * @brief 文件查看视图（/view 只读）：路径栏 + 行号 + 虚拟化滚动 + 状态栏
 * @details 纯渲染组件：状态由调用方（App）持有，本组件只负责渲染。
 *          文件行高固定 1，scroll 偏移 + 可视行数切片实现虚拟化滚动。
 */

#pragma once

#include <ftxui/dom/elements.hpp>

#include "vm/view_model.h"

namespace ftxtui {

/// @brief 构建文件查看视图
/// @param file 文件 tab 状态（path 为空 = 未打开，显示占位）
ftxui::Element build_file_viewer(const FileViewState& file);

}  // namespace ftxtui
