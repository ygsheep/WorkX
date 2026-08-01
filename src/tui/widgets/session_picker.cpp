/**
 * @file session_picker.cpp
 * @brief 会话选择面板实现
 * @details 全屏 overlay + Screen 差分渲染，Claude Code 风格搜索框 + 双行会话列表
 * @version 1.1.0
 * @date 2026-07
 */

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
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

/// 每条会话占 3 行（标题 + 副信息 + 空行），最多显示 5 条
constexpr int MAX_DISPLAY_ITEMS = 5;
constexpr int ITEM_HEIGHT = 3;  // 标题行 + 副信息行 + 空行

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

/// 相对时间格式化（file_time_type → "X周前" 风格）
std::string relative_time(std::filesystem::file_time_type ftime) {
    try {
        auto now = std::filesystem::file_time_type::clock::now();
        auto diff = now - ftime;
        auto secs = std::chrono::duration_cast<std::chrono::seconds>(diff).count();

        if (secs < 60) return "刚刚";
        auto mins = secs / 60;
        if (mins < 60) return std::format("{}分钟前", mins);
        auto hours = mins / 60;
        if (hours < 24) return std::format("{}小时前", hours);
        auto days = hours / 24;
        if (days < 7) return std::format("{}天前", days);
        auto weeks = days / 7;
        if (weeks < 4) return std::format("{}周前", weeks);
        auto months = days / 30;
        if (months < 12) return std::format("{}个月前", months);
        auto years = days / 365;
        return std::format("{}年前", years);
    } catch (...) {
        return "未知时间";
    }
}

/// 文件大小格式化
std::string format_size(uintmax_t bytes) {
    if (bytes < 1024) return std::format("{} B", bytes);
    double kb = static_cast<double>(bytes) / 1024.0;
    if (kb < 1024.0) return std::format("{:.1f} KB", kb);
    double mb = kb / 1024.0;
    return std::format("{:.1f} MB", mb);
}

/// 获取文件大小（安全）
std::string get_file_size(const std::string& path) {
    std::error_code ec;
    auto size = std::filesystem::file_size(path, ec);
    if (ec) return "未知大小";
    return format_size(size);
}

/// 截断字符串到指定显示宽度（按 UTF-8 字符数）
std::string truncate_utf8(const std::string& s, size_t max_chars) {
    if (max_chars == 0) return "";
    size_t char_count = 0;
    size_t byte_pos = 0;
    while (char_count < max_chars && byte_pos < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[byte_pos]);
        if (c < 0x80) byte_pos += 1;
        else if ((c & 0xE0) == 0xC0) byte_pos += 2;
        else if ((c & 0xF0) == 0xE0) byte_pos += 3;
        else if ((c & 0xF8) == 0xF0) byte_pos += 4;
        else byte_pos += 1;
        ++char_count;
    }
    std::string result = s.substr(0, byte_pos);
    if (byte_pos < s.size()) result += "…";
    return result;
}

} // anonymous namespace

std::string pick_session_interactive(
    Terminal* term, Screen* scr,
    const std::string& project_dir)
{
    // 加载会话列表（可能为空，空列表也显示面板，列表区提示"没有可恢复会话"）
    auto all_sessions = session::SessionStore::list_sessions(project_dir);

    // 过滤后的会话列表（根据搜索词）
    std::vector<session::SessionMeta> filtered = all_sessions;
    std::string search_query;
    int selected = 0;
    int scroll_offset = 0;

    // 开启 overlay
    int term_height = term->get_terminal_height();
    int overlay_bottom = term_height - 2;
    if (overlay_bottom < 1) overlay_bottom = 1;
    term->begin_overlay(1, overlay_bottom);

    auto apply_filter = [&]() {
        filtered.clear();
        for (const auto& s : all_sessions) {
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

        // ===== 标题 =====
        scr->write(row, 0, "  Resume session", ColorRole::StatusBar);
        row++;

        // ===== 搜索框（三行：上边框 / 内容行 / 下边框）=====
        // Screen::write 按显示列计算位置（每个 UTF-8 字符占 1 列），需按列数对齐。
        const std::string HORIZ = "─";  // U+2500，显示占 1 列
        const std::string VERT = "│";   // U+2502，显示占 1 列
        const std::string TL = "╭", TR = "╮", BL = "╰", BR = "╯";  // 各占 1 列

        // 总列预算 = width。"  "(2) + ╭(1) + N×─(N) + ╮(1) = 4 + N ≤ width
        int horiz_count = width - 4;
        if (horiz_count < 1) horiz_count = 1;

        // 上边框：  ╭────╮
        std::string top_border = "  " + TL;
        for (int i = 0; i < horiz_count; ++i) top_border += HORIZ;
        top_border += TR;
        scr->write(row, 0, top_border, ColorRole::Dim);
        row++;

        // 内容行：  │ Search... │
        // 右 │ 的目标列位置 = 2(缩进) + 1(│) + horiz_count(内容区) = 3 + horiz_count
        // 内容行已用列 = 2(缩进) + 1(│) + 1(空格) + search_text 列数
        // padding = 目标列 - 已用列
        std::string search_text = search_query.empty() ? "Search..." : search_query;
        int right_col = 3 + horiz_count;
        int used_cols = 4 + static_cast<int>(search_text.size());  // 2+1+1+text
        int fill_cols = right_col - used_cols;
        if (fill_cols < 1) fill_cols = 1;  // 至少 1 空格
        // 截断过长的 search_text
        int max_search_cols = right_col - 4 - 1;  // 留至少 1 空格
        if (max_search_cols < 1) max_search_cols = 1;
        if (static_cast<int>(search_text.size()) > max_search_cols) {
            search_text = search_text.substr(0, max_search_cols);
            used_cols = 4 + static_cast<int>(search_text.size());
            fill_cols = right_col - used_cols;
            if (fill_cols < 1) fill_cols = 1;
        }
        std::string content_line = "  " + VERT + " " + search_text;
        for (int i = 0; i < fill_cols; ++i) content_line += " ";
        content_line += VERT;
        ColorRole search_color = search_query.empty() ? ColorRole::Dim : ColorRole::Prompt;
        scr->write(row, 0, content_line, search_color);
        row++;

        // 下边框：  ╰────╯
        std::string bottom_border = "  " + BL;
        for (int i = 0; i < horiz_count; ++i) bottom_border += HORIZ;
        bottom_border += BR;
        scr->write(row, 0, bottom_border, ColorRole::Dim);
        row++;

        // 空行
        row++;

        // ===== 会话列表（双行条目）=====
        if (filtered.empty()) {
            // 区分两种空状态：无历史会话 vs 搜索无匹配
            std::string empty_hint = all_sessions.empty()
                ? "    没有可恢复会话"
                : "    (无匹配会话)";
            scr->write(row, 0, empty_hint, ColorRole::Dim);
            row++;
        } else {
            // 计算可见区域剩余行数
            int remaining_rows = overlay_bottom - row - 1;  // 留 1 行给底部提示
            int max_items = std::min(MAX_DISPLAY_ITEMS, remaining_rows / ITEM_HEIGHT);
            if (max_items < 1) max_items = 1;

            // 调整 scroll_offset 使选中项可见
            if (selected < scroll_offset) scroll_offset = selected;
            if (selected >= scroll_offset + max_items) {
                scroll_offset = selected - max_items + 1;
            }

            for (int i = 0; i < max_items; ++i) {
                int idx = scroll_offset + i;
                if (idx >= static_cast<int>(filtered.size())) break;

                const auto& s = filtered[idx];
                std::string title = s.title.empty() ? "未命名会话" : s.title;
                std::string branch = s.git_branch.empty() ? "no-branch" : s.git_branch;
                std::string rel_time = relative_time(s.last_modified);
                std::string size_str = get_file_size(s.file_path);

                // 选中项用 ❯ 标识，未选中用空格
                std::string marker = (idx == selected) ? "  ❯ " : "    ";
                ColorRole title_color = (idx == selected) ? ColorRole::StatusBar : ColorRole::Default;

                // 标题行（截断到 width - marker 长度）
                int max_title_chars = width - static_cast<int>(marker.size()) - 2;
                if (max_title_chars < 1) max_title_chars = 1;
                std::string title_line = marker + truncate_utf8(title, static_cast<size_t>(max_title_chars));
                if (static_cast<int>(title_line.size()) > width) {
                    title_line = title_line.substr(0, width);
                }
                scr->write(row, 0, title_line, title_color);
                row++;

                // 副信息行：相对时间 · 分支 · 大小
                std::string sub_line = "      " + rel_time + " · " + branch + " · " + size_str;
                if (static_cast<int>(sub_line.size()) > width) {
                    sub_line = sub_line.substr(0, width);
                }
                scr->write(row, 0, sub_line, ColorRole::Dim);
                row++;

                // 空行分隔
                row++;
            }

            // 滚动指示
            if (static_cast<int>(filtered.size()) > max_items) {
                std::string hint = std::format("    ({}/{})", selected + 1, filtered.size());
                if (row < overlay_bottom) {
                    scr->write(row, 0, hint, ColorRole::Dim);
                }
            }
        }

        // ===== 底部提示 =====
        if (row < overlay_bottom) {
            row = overlay_bottom - 1;
        }
        std::string footer = "  输入搜索 · Enter 选择 · Esc 清空";
        if (static_cast<int>(footer.size()) > width) {
            footer = footer.substr(0, width);
        }
        scr->write(row, 0, footer, ColorRole::Dim);

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
        } else if (key == KEY_ESC) {
            if (search_query.empty()) {
                // 搜索框已空，Esc 退出
                term->end_overlay();
                scr->reset_buffers();
                return "";
            } else {
                // 搜索框非空，Esc 清空搜索
                search_query.clear();
                apply_filter();
            }
        } else if (key == KEY_CTRL_C) {
            // Ctrl+C 直接退出
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
