/**
 * @file command_panel.h
 * @brief 命令面板
 * @details 当用户输入 / 时，Status Bar 区域切换为命令面板，
 *          支持上下选择、Tab 补全、实时过滤
 * @version 1.0.0
 */

#pragma once

#include <string>
#include <vector>
#include <functional>

namespace agent {

class Terminal;

/// @brief 命令条目
struct CommandEntry {
    std::string name;        ///< "help"
    std::string description; ///< "Show available commands"
    std::string usage;       ///< "/help"
};

/// @brief 命令面板
/// @details 占据 Status Bar 所在行（屏幕最后一行），
///          显示过滤后的命令列表，支持上下选择和 Tab 补全
class CommandPanel {
public:
    explicit CommandPanel(Terminal* terminal);

    /// @brief 设置可用命令列表
    void set_commands(const std::vector<CommandEntry>& commands);

    /// @brief 更新过滤前缀（如 "/he" → 只显示 help）
    void set_filter(const std::string& prefix);

    /// @brief 上移选择
    void move_up();

    /// @brief 下移选择
    void move_down();

    /// @brief 获取当前选中命令的 Tab 补全文本（如 "/help "）
    /// @return 补全文本，空字符串表示无匹配
    std::string get_completion() const;

    /// @brief 获取当前选中命令的完整输入（如 "/help"）
    const CommandEntry* get_selected() const;

    /// @brief 渲染命令面板到 Status Bar 区域
    void render();

    /// @brief 清除命令面板（恢复 Status Bar 占位）
    void clear();

    /// @brief 面板是否活跃（有匹配命令）
    bool is_active() const { return m_active; }

    /// @brief 设置面板是否可见
    void set_visible(bool visible);

    /// @brief 面板是否可见
    bool is_visible() const { return m_visible; }

private:
    /// @brief 构建过滤后的索引列表
    void rebuild_filtered();

    Terminal* m_terminal;

    std::vector<CommandEntry> m_all_commands;
    std::vector<size_t> m_filtered;  ///< 过滤后的命令索引
    int m_selected = 0;               ///< 当前选中（在 m_filtered 中的索引）
    std::string m_filter;             ///< 当前过滤前缀（不含 /）

    bool m_visible = false;           ///< 面板是否可见
    bool m_active = false;            ///< 是否有匹配命令

    std::string m_last_rendered;      ///< 上次渲染内容（差分比较）
};

} // namespace agent
