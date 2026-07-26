/**
 * @file select_panel.h
 * @brief 选择面板
 * @details Tab 分组 + 多选 UI，使用 Screen 差分渲染
 *          [Tab1] [Tab2]  ← Tab 键切换
 *          ◉ item1        ← 空格选中/取消
 *          ● item2        ← ●=光标 ◉=选中 ○=未选
 * @version 1.0.0
 */

#pragma once

#include <string>
#include <vector>
#include "tui/core/screen.h"

namespace tui {

class Terminal;
class IPlatform;

/// @brief 选择项
struct SelectItem {
    std::string id;       ///< 唯一标识
    std::string display;  ///< 显示文本
    bool selected = false; ///< 是否选中
};

/// @brief 选择 Tab 分组
struct SelectTab {
    std::string name;                ///< Tab 名称
    std::vector<SelectItem> items;   ///< 选项列表
};

/// @brief 选择面板
/// @details 覆盖输出区底部几行，使用 Screen 差分渲染
class SelectPanel {
public:
    SelectPanel(Terminal* terminal, Screen* screen);

    /// @brief 设置 Tab 列表
    void set_tabs(const std::vector<SelectTab>& tabs);

    /// @brief Tab 切换（向右）
    void next_tab();

    /// @brief Tab 切换（向左）
    void prev_tab();

    /// @brief 上移光标
    void move_up();

    /// @brief 下移光标
    void move_down();

    /// @brief 空格选中/取消当前项
    void toggle_current();

    /// @brief 获取所有选中项的 id
    std::vector<std::string> get_selected_ids() const;

    /// @brief 获取当前光标所在项
    const SelectItem* get_current_item() const;

    // ---- 内联输入模式 ----

    /// @brief 激活内联输入模式（清除已有输入）
    void activate_input_mode();

    /// @brief 退出内联输入模式
    void deactivate_input_mode();

    /// @brief 是否处于内联输入模式
    bool is_input_mode() const;

    /// @brief 获取输入的文本
    const std::string& get_input_text() const;

    /// @brief 追加一个 Unicode 字符到输入缓冲区
    void input_char(char32_t ch);

    /// @brief 删除最后一个 UTF-8 码点
    void input_backspace();

    /// @brief 设置标题
    void set_title(const std::string& title);

    /// @brief 获取面板占用的行数
    int panel_height() const;

    /// @brief 渲染面板
    void render();

    /// @brief 清除面板（恢复终端）
    void dismiss();

private:
    Terminal* m_terminal;
    Screen* m_screen;

    std::vector<SelectTab> m_tabs;
    int m_active_tab = 0;
    int m_cursor_row = 0;    ///< 当前光标在 items 中的索引
    std::string m_title;

    bool m_input_mode = false;        ///< 内联输入是否活跃
    std::string m_input_buffer;       ///< 用户已输入的文本
};

} // namespace tui
