/**
 * @file composer.h
 * @brief 底部输入组件（ftxui::Input + 快捷键处理）
 * @details Enter 提交；Shift+Tab 切换权限；Ctrl+P 命令面板。
 *          MVP 为单行输入（ftxui Input 原生不支持多行 textarea），后续可用
 *          Renderer 自绘多行编辑器替代。
 */

#pragma once

#include <functional>
#include <string>

#include <ftxui/component/component.hpp>

namespace ftxtui {

/// @brief composer 回调与状态
struct ComposerOptions {
    std::string* buffer = nullptr;                      ///< 输入缓冲（Input 直连）
    size_t* cursor = nullptr;                           ///< 光标位置（外部持有，可为空）
    std::function<void(const std::string&)> on_submit;  ///< 提交（Enter）
    std::function<void()> on_perm_toggle;               ///< 权限切换（Shift+Tab）
    std::function<void()> on_toggle_thinking;           ///< 思考视图（Ctrl+O）

    // ---- 输入栏提示面板（App 侧状态机；返回 true = 已消费）----
    std::function<bool()> suggest_active;   ///< 面板是否激活
    std::function<void(int)> suggest_move;  ///< 移动选择（±1；Tab 传 +1，App 循环）
    std::function<bool()> suggest_enter;    ///< Enter 确认面板动作
    std::function<void()> suggest_cancel;   ///< Esc 关闭面板
    std::function<void()> suggest_refresh;  ///< 字符 / 退格后刷新过滤
};

/// @brief 构建 composer 组件
ftxui::Component make_composer(ComposerOptions& opt);

}  // namespace ftxtui