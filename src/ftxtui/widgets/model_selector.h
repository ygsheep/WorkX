/**
 * @file model_selector.h
 * @brief 模型切换模态（ftxui::Menu）
 */

#pragma once

#include <functional>
#include <string>
#include <vector>

#include <ftxui/component/component.hpp>

namespace ftxtui {

/// @brief 构建模型选择面板
/// @param models 模型名列表
/// @param on_select 选中回调（Enter 确认，参数为索引）
/// @param open 开关
ftxui::Component make_model_selector(
    std::vector<std::string>& models,
    std::function<void(int)> on_select,
    bool& open);

}  // namespace ftxtui