/**
 * @file file_search_panel.h
 * @brief 文件搜索面板
 * @details 当用户输入 @ 时，Status Bar 区域切换为文件搜索面板，
 *          显示匹配的文件列表，支持上下选择、Tab 补全
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <vector>
#include "core/utils/file_index.h"

namespace tui {

class Terminal;

/// @brief 文件搜索面板
/// @details 占据 Status Bar 所在行（屏幕最后一行上方），
///          显示文件索引中匹配的文件列表，支持上下选择和 Tab 补全
class FileSearchPanel {
public:
    explicit FileSearchPanel(Terminal* terminal);

    /// @brief 设置搜索查询（@ 后的文本）
    /// @param query 搜索查询
    void set_query(const std::string& query);

    /// @brief 上移选择
    void move_up();

    /// @brief 下移选择
    void move_down();

    /// @brief 获取当前选中文件的路径（目录带尾部 /）
    /// @return 文件路径，空字符串表示无匹配
    std::string get_selected_path() const;

    /// @brief 渲染文件搜索面板
    void render();

    /// @brief 清除面板（恢复 Status Bar 占位）
    void clear();

    /// @brief 面板是否活跃（有匹配文件）
    bool is_active() const { return m_active; }

    /// @brief 设置面板是否可见
    void set_visible(bool visible);

    /// @brief 面板是否可见
    bool is_visible() const { return m_visible; }

private:
    /// @brief 从文件索引搜索并更新结果
    void search_files();

    Terminal* m_terminal;

    std::string m_query;                              ///< 搜索查询
    std::vector<agent::FileIndex::Entry> m_results;          ///< 搜索结果
    int m_selected = 0;                               ///< 当前选中索引

    bool m_visible = false;                    ///< 面板是否可见
    bool m_active = false;                     ///< 是否有匹配文件

    std::string m_last_rendered;               ///< 上次渲染内容（差分比较）
};

} // namespace tui
