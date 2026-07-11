/**
 * @file bottom_bar_manager.h
 * @brief 底部区域管理器
 * @details 统一管理 StatusBar / CommandPanel / SelectPanel 三种模式的切换
 *          它们共享屏幕底部区域，同一时间只有一种处于活跃状态
 * @version 1.0.0
 */

#pragma once

#include <string>
#include <vector>
#include <memory>
#include "tui/core/tui_state.h"

namespace workx {

class Terminal;
class StatusBar;
class CommandPanel;
class SelectPanel;
class Screen;

/// @brief 底部区域模式
enum class BottomBarMode {
    STATUS_BAR,      ///< 默认状态栏
    COMMAND_PANEL,   ///< 命令面板（输入 / 触发）
    SELECT_PANEL,    ///< 选择面板（/model 等命令触发）
};

/// @brief 底部区域管理器
class BottomBarManager {
public:
    explicit BottomBarManager(Terminal* terminal);
    ~BottomBarManager();

    /// @brief 初始化（创建 StatusBar、CommandPanel 等）
    void initialize(Screen* screen);

    /// @brief 设置外部 StatusBar（如 ChatRenderer 的 StatusBar），优先于内部实例使用
    void set_status_bar(StatusBar* sb);

    /// @brief 切换模式
    void set_mode(BottomBarMode mode);

    /// @brief 获取当前模式
    BottomBarMode mode() const { return m_mode; }

    /// @brief 渲染当前活跃的面板
    void render();

    // ---- StatusBar 委托 ----
    StatusBar& status_bar() { return *get_status_bar(); }
    const StatusBar& status_bar() const { return *get_status_bar(); }

    // ---- CommandPanel 委托 ----
    CommandPanel& command_panel() { return *m_command_panel; }
    const CommandPanel& command_panel() const { return *m_command_panel; }

    // ---- SelectPanel 委托 ----
    SelectPanel& select_panel() { return *m_select_panel; }
    const SelectPanel& select_panel() const { return *m_select_panel; }

    /// @brief 输入行内容变化时的通知
    /// @param line 当前行内容
    void on_input_changed(const std::string& line);

    /// @brief 处理方向键（在 CommandPanel 活跃时拦截）
    /// @param key 方向键码
    /// @return true 如果已处理（不应继续传递给 history）
    bool handle_navigation(char32_t key);

    /// @brief 处理 Tab 键（在 CommandPanel 活跃时拦截）
    /// @return 补全文本，空字符串表示未处理
    std::string handle_tab();

private:
    StatusBar* get_status_bar() const;

    Terminal* m_terminal;
    BottomBarMode m_mode = BottomBarMode::STATUS_BAR;

    std::unique_ptr<StatusBar> m_status_bar;
    StatusBar* m_external_sb = nullptr;  ///< 外部 StatusBar（如 ChatRenderer 的），优先使用
    std::unique_ptr<CommandPanel> m_command_panel;
    std::unique_ptr<SelectPanel> m_select_panel;
};

} // namespace workx
