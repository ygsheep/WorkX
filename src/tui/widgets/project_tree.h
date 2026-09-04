/**
 * @file project_tree.h
 * @brief 项目文件树视图（「项目」tab，常驻）：文件列表 + JetBrains 图标 + git 状态点
 * @details 扁平可视行 + 虚拟化滚动；目录行点击展开/收起（twisty + 文件夹图标），
 *          文件行点击经回调打开（复用 /view）。纯渲染组件状态由调用方（App）持有。
 */

#pragma once

#include <functional>

#include <ftxui/component/component.hpp>
#include <ftxui/dom/elements.hpp>

#include "vm/view_model.h"

namespace ftxtui {

/// @brief 扁平可视行（由树 DFS 生成；node 指向 ViewModel 持有树节点，UI 线程内稳定）
struct ProjectRow {
    const ProjectNode* node = nullptr;  ///< 指向树节点（展开折叠不改结构，地址稳定）
    int depth = 0;                      ///< 缩进层级
};

/// @brief 生成项目文件树扁平可视行（DFS，仅展开目录递归；目录优先 + 名称排序）
/// @param tree 项目根 children
std::vector<ProjectRow> flatten_project_rows(const std::vector<ProjectNode>& tree);

/// @brief 构建项目文件树 tab 组件（可聚焦：鼠标点击切换目录/打开文件、滚轮滚动）
/// @param project 项目树状态（App 持有）
/// @param on_open_file 点击文件行回调（App 打开 /view；参数为文件相对项目根路径）
ftxui::Component make_project_tree(ProjectTreeState* project,
                                   std::function<void(const std::string& rel_path)> on_open_file);

/// @brief 项目文件树视图渲染（纯元素；供无头测试与未聚焦时回退渲染）
/// @param project 项目树状态
ftxui::Element build_project_tree(const ProjectTreeState& project);

}  // namespace ftxtui