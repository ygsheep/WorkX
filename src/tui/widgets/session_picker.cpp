/**
 * @file session_picker.cpp
 * @brief 会话选择面板实现
 * @details 全屏 overlay + Screen 差分渲染，搜索框 + 会话列表
 * @version 1.0.0
 * @date 2026-07
 */

#include <algorithm>
#include <cctype>
#include <format>
#include <string>

#include "tui/widgets/session_picker.h"
#include "tui/core/screen.h"
#include "tui/core/terminal.h"
#include "tui/core/color_scheme.h"
#include "tui/core/platform/i_platform.h"

namespace agent {

using namespace tui;

namespace {

/// 特殊按键码（与 model_selector.cpp 一致）
constexpr char32_t KEY_UP    = 0xE002;
constexpr char32_t KEY_DOWN  = 0xE003;
constexpr char32_t KEY_ENTER = 0x0D;
constexpr char32_t KEY_ESC   = 0x1B;
constexpr char32_t KEY_CTRL_C = 0xE009;
constexpr char32_t KEY_BACKSPACE = 0x08;
constexpr char32_t KEY_DELETE = 0x7F;

/// 最多显示的会话条目数
constexpr int MAX_DISPLAY = 7;

/// 不区分大小写包含检查
bool icontains(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    if (haystack.size() < needle.size()) return false;
    auto it = std::search(haystack.begin(), haystack.end(),
                          needle.begin(), needle.end(),
                          [](char a, char b) {
                              return std::tolower(static_cast<unsigned char>(a)) ==
                                     std::tolower(static_cast<unsigned char>(b));
                          });
    return it != haystack.end();
}

/// 简化时间显示（ISO 8601 → YYYY-MM-DD HH:MM）
std::string simplify_time(const std::string& iso) {
    if (iso.size() >= 16) {
        return iso.substr(0, 10) + " " + iso.substr(11, 5);
    }
    return iso;
}

} // anonymous namespace

std::string pick_session_interactive(
    Terminal* term, Screen* scr,
    const std::string& project_dir)
{
    // 加载会话列表
    auto all_sessions = session::SessionStore::list_sessions(project_dir);

    if (all_sessions.empty()) {
        // 无历史会话，直接返回空
        return "";
    }

    // 过滤后的会话列表（根据搜索词）
    std::vector<session::SessionMeta> filtered = all_sessions;
    std::string search_query;
    int selected = 0;
    int scroll_offset = 0;

    // 开启 overlay
    int scroll_bottom = term->get_terminal_height() - 3;
    if (scroll_bottom < 1) scroll_bottom = 1;
    term->begin_overlay(1, scroll_bottom);

    auto apply_filter = [&]() {
        filtered.clear();
        for (const auto& s : all_sessions) {
            // 按标题、分支、模型过滤
            if (icontains(s.title, search_query) ||
                icontains(s.git_branch, search_query) ||
                icontains(s.model, search_query)) {
                filtered.push_back(s);
            }
        }
        if (selected >= static_cast<int>(filtered.size())) {
            selected = 0;
        }
        scroll_offset = 0;
    };

    auto do_render = [&]() {
        scr->clear();

        int row = 0;
        int width = scr->width();
        if (width < 40) width = 40;

        // 标题行
        scr->write(row, 0, "  ====== 切换历史会话 ======", ColorRole::StatusBar);
        row++;

        // 搜索框
        std::string search_line = "  搜索: " + search_query + "_";
        scr->write(row, 0, search_line, ColorRole::Prompt);
        row++;

        // 分隔线
        scr->write(row, 0, std::string("  ──────────────────────────────────────────────").substr(0, width),
                   ColorRole::Dim);
        row++;

        if (filtered.empty()) {
            scr->write(row, 0, "  (无匹配会话)", ColorRole::Dim);
        } else {
            int display_count = std::min(MAX_DISPLAY, static_cast<int>(filtered.size()));
            // 调整 scroll_offset 使选中项可见
            if (selected < scroll_offset) scroll_offset = selected;
            if (selected >= scroll_offset + display_count) {
                scroll_offset = selected - display_count + 1;
            }

            for (int i = 0; i < display_count; ++i) {
                int idx = scroll_offset + i;
                if (idx >= static_cast<int>(filtered.size())) break;

                const auto& s = filtered[idx];
                std::string branch = s.git_branch.empty() ? "no-branch" : s.git_branch;
                std::string time = simplify_time(s.created_at);
                std::string title = s.title.empty() ? "未命名会话" : s.title;

                // 格式：  标题 (时间 | 分支 | N条消息)
                std::string line = std::format("  {} ({} | {} | {} 条消息)",
                                               title, time, branch, s.message_count);
                if (static_cast<int>(line.size()) > width) {
                    line = line.substr(0, width);
                }

                ColorRole color = (idx == selected) ? ColorRole::StatusBar : ColorRole::Default;
                if (idx == selected) {
                    // 选中行反色（用 StatusBar 色调）
                    // 补齐到 width 防止残留
                    line += std::string(width - line.size(), ' ');
                }
                scr->write(row, 0, line, color);
                row++;
            }

            // 滚动提示
            if (static_cast<int>(filtered.size()) > MAX_DISPLAY) {
                std::string hint = std::format("  ({}/{})", selected + 1, filtered.size());
                scr->write(row, 0, hint, ColorRole::Dim);
            }
        }

        row++;
        // 底部提示
        scr->write(row, 0, "  ↑↓ 选择 | Enter 确认 | Esc 取消 | 输入搜索", ColorRole::Dim);

        scr->flush();
    };

    // 主交互循环
    while (true) {
        do_render();
        char32_t key = term->platform()->read_char();

        if (key == KEY_UP) {
            if (selected > 0) selected--;
        } else if (key == KEY_DOWN) {
            if (selected < static_cast<int>(filtered.size()) - 1) selected++;
        } else if (key == KEY_ENTER) {
            // 确认选择
            if (!filtered.empty() && selected < static_cast<int>(filtered.size())) {
                std::string path = filtered[selected].file_path;
                term->end_overlay();
                scr->reset_buffers();
                return path;
            }
        } else if (key == KEY_ESC || key == KEY_CTRL_C) {
            // 取消
            term->end_overlay();
            scr->reset_buffers();
            return "";
        } else if (key == KEY_BACKSPACE || key == KEY_DELETE) {
            if (!search_query.empty()) {
                search_query.pop_back();
                apply_filter();
            }
        } else if (key >= 0x20 && key <= 0x7E) {
            // 可打印 ASCII 字符
            search_query += static_cast<char>(key);
            apply_filter();
        }
        // 其他特殊键忽略
    }
}

} // namespace agent
