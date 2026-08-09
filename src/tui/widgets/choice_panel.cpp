/**
 * @file choice_panel.cpp
 * @brief ChoicePanel — 多 Tab 单选/多选面板实现
 * @version 1.0.0
 * @date 2026-08
 */

#include "tui/widgets/choice_panel.h"
#include "tui/core/terminal.h"
#include "tui/core/screen.h"
#include "tui/core/color_scheme.h"
#include "tui/core/platform/i_platform.h"

#include <format>
#include <algorithm>
#include <sstream>

namespace tui {

namespace {

// 按键码（与 platform_win32.cpp 保持一致）
constexpr char32_t KEY_UP    = 0xE002;
constexpr char32_t KEY_DOWN  = 0xE003;
constexpr char32_t KEY_LEFT  = 0xE000;
constexpr char32_t KEY_RIGHT = 0xE001;
constexpr char32_t KEY_ENTER = 0x0D;
constexpr char32_t KEY_ESC   = 0x1B;
constexpr char32_t KEY_SPACE = 0x20;
constexpr char32_t KEY_CTRL_C = 0xE009;
constexpr char32_t KEY_BACKSPACE = 0x08;
constexpr char32_t KEY_DELETE = 0x7F;
// KEY_WAKE 由 i_platform.h 统一导出（跨线程唤醒，AskUser 超时等）

// 显示常量
constexpr int MAX_DISPLAY_ITEMS = 8;   ///< 最多显示的选项数（超出滚动）
constexpr int LEFT_PADDING = 2;         ///< 左侧缩进
constexpr int TOP_MARGIN = 1;           ///< 顶部留白
constexpr int BOTTOM_MARGIN = 1;        ///< 底部留白（提示行上方）

/// @brief 截断 UTF-8 字符串到指定显示列数
std::string truncate_utf8_display(const std::string& s, int max_cols) {
    int cols = 0;
    size_t byte_pos = 0;
    while (byte_pos < s.size()) {
        unsigned char c = static_cast<unsigned char>(s[byte_pos]);
        int char_bytes, char_cols;
        if (c < 0x80) { char_bytes = 1; char_cols = 1; }
        else if ((c & 0xE0) == 0xC0) { char_bytes = 2; char_cols = 1; }
        else if ((c & 0xF0) == 0xE0) { char_bytes = 3; char_cols = 1; }  // CJK 简化为 1
        else if ((c & 0xF8) == 0xF0) { char_bytes = 4; char_cols = 1; }
        else { char_bytes = 1; char_cols = 1; }

        if (cols + char_cols > max_cols) break;
        byte_pos += char_bytes;
        cols += char_cols;
    }
    return s.substr(0, byte_pos);
}

} // namespace

// ============================================================
// ChoiceResult::to_json
// ============================================================
std::string ChoiceResult::to_json() const {
    nlohmann::json j;
    j["status"] = submitted ? "submitted" : "cancelled";
    if (submitted) {
        nlohmann::json ans = nlohmann::json::object();
        for (const auto& [question, answer] : answers) {
            ans[question] = answer;
        }
        j["answers"] = ans;
    }
    return j.dump(2);
}

// ============================================================
// parse_choice_config
// ============================================================
std::optional<ChoiceConfig> parse_choice_config(const nlohmann::json& input) {
    try {
        if (!input.contains("questions") || !input["questions"].is_array()) return std::nullopt;

        ChoiceConfig config;

        for (const auto& q_j : input["questions"]) {
            if (!q_j.contains("question") || !q_j["question"].is_string()) return std::nullopt;
            if (!q_j.contains("header") || !q_j["header"].is_string()) return std::nullopt;
            if (!q_j.contains("options") || !q_j["options"].is_array()) return std::nullopt;

            ChoiceTab tab;
            tab.question = q_j["question"].get<std::string>();
            tab.header = q_j["header"].get<std::string>();
            tab.multi = q_j.value("multiSelect", false);
            tab.allow_custom_input = q_j.value("allow_custom_input", true);

            for (const auto& opt_j : q_j["options"]) {
                if (!opt_j.contains("label") || !opt_j["label"].is_string()) return std::nullopt;
                ChoiceItem item;
                item.label = opt_j["label"].get<std::string>();
                item.id = item.label;  // 对齐 cc：label 即标识
                if (opt_j.contains("description") && opt_j["description"].is_string()) {
                    item.description = opt_j["description"].get<std::string>();
                }
                tab.items.push_back(std::move(item));
            }

            if (tab.items.empty()) return std::nullopt;
            config.tabs.push_back(std::move(tab));
        }

        if (config.tabs.empty()) return std::nullopt;
        return config;
    } catch (...) {
        return std::nullopt;
    }
}

// ============================================================
// run_choice_panel
// ============================================================
ChoiceResult run_choice_panel(Terminal* term, Screen* scr, const ChoiceConfig& config,
                               const std::atomic<bool>* cancel_flag) {
    if (config.tabs.empty()) {
        return ChoiceResult{};
    }

    // 拷贝配置为可变状态
    std::vector<ChoiceTab> tabs = config.tabs;
    int current_tab = 0;
    int scroll_offset = 0;

    // 为 allow_custom_input 的 Tab 追加"自定义输入"选项
    // 显示文本: ✎ 自定义输入...
    for (auto& tab : tabs) {
        if (tab.allow_custom_input) {
            ChoiceItem custom_item;
            custom_item.id = "__custom__";
            custom_item.label = "\xe2\x9c\x8e \xe8\x87\xaa\xe5\xae\x9a\xe4\xb9\x89\xe8\xbe\x93\xe5\x85\xa5...";  // ✎ 自定义输入...
            custom_item.is_custom_input = true;
            tab.items.push_back(std::move(custom_item));
        }
    }

    // 输入模式状态
    bool input_mode = false;
    std::string input_buffer;

    // ===== 开启 overlay =====
    int term_width = term->get_terminal_width();
    int term_height = term->get_terminal_height();
    int overlay_bottom = term_height - 2;
    if (overlay_bottom < 1) overlay_bottom = 1;
    term->begin_overlay(1, overlay_bottom);

    // 差分渲染对齐终端实际尺寸（Screen 默认 80x24，不 resize 会渲染截断/越界，
    // 破坏 overlay 区域外的状态栏/输入行且 end_overlay 不恢复）
    scr->resize(term_width, term_height);

    // 清空 overlay 区域：差分渲染首帧不会输出"空白行"（m_previous 同为空白），
    // 若不显式清行会残留旧聊天内容（"没有清屏"）；结束后由 end_overlay() 从快照恢复
    for (int r = 1; r <= overlay_bottom; ++r) {
        char pos_cmd[32];
        snprintf(pos_cmd, sizeof(pos_cmd), "\x1b[%d;1H\x1b[2K", r);
        term->write(pos_cmd);
    }
    term->write("\x1b[1;1H");  // 光标归位 overlay 顶部，等待 Screen 差分渲染

    // ===== 渲染函数 =====
    auto do_render = [&]() {
        scr->clear();
        int width = std::max(scr->width(), 40);

        int row = TOP_MARGIN;

        // 1. 标题行（用当前 Tab 的完整问题文本作为标题）
        {
            const auto& cur_q = config.tabs[current_tab];
            std::string title_line = std::string(LEFT_PADDING, ' ') + cur_q.question;
            scr->write(row, 0, title_line, ColorRole::StatusBar);
            // 标题下方分隔线（满宽连边，使用 ─ U+2500）
            ++row;
            std::string sep;
            sep.reserve(width * 3);
            for (int i = 0; i < width; ++i) sep += "\xe2\x94\x80";  // ─
            scr->write(row, 0, sep, ColorRole::Dim);
            ++row;
        }

        // 2. Tab 栏（选中 Tab 蓝底背景，不用方括号；用 header 作为短标签）
        {
            int col = LEFT_PADDING;
            for (int i = 0; i < static_cast<int>(tabs.size()); ++i) {
                // Tab 之间用 2 空格分隔
                if (i > 0) {
                    scr->write(row, col, "  ", ColorRole::Default);
                    col += 2;
                }
                // 选中：蓝底白字 " header "；未选中：灰色 " header "
                std::string segment = " " + tabs[i].header + " ";
                ColorRole seg_color = (i == current_tab)
                    ? ColorRole::ChoiceTabActiveBg
                    : ColorRole::SelectTab;
                scr->write(row, col, segment, seg_color);
                col += static_cast<int>(segment.size());
            }
            ++row;
            // Tab 下方分隔线（满宽连边，使用 ─ U+2500）
            std::string sep;
            sep.reserve(width * 3);
            for (int i = 0; i < width; ++i) sep += "\xe2\x94\x80";  // ─
            scr->write(row, 0, sep, ColorRole::Dim);
            ++row;
        }

        // 3. 选项列表
        const auto& cur_tab = tabs[current_tab];
        int max_items = std::min(MAX_DISPLAY_ITEMS, overlay_bottom - row - 2);

        // 调整 scroll_offset 使 cursor 可见
        if (cur_tab.cursor < scroll_offset) scroll_offset = cur_tab.cursor;
        if (cur_tab.cursor >= scroll_offset + max_items) {
            scroll_offset = cur_tab.cursor - max_items + 1;
        }

        for (int i = 0; i < max_items && scroll_offset + i < static_cast<int>(cur_tab.items.size()); ++i) {
            const auto& item = cur_tab.items[scroll_offset + i];
            bool is_cursor = (scroll_offset + i == cur_tab.cursor);

            // [√] 或 [ ] 前缀
            std::string prefix = item.selected ? "[\xe2\x88\x9a]" : "[ ]";  // √ = U+221A
            std::string num = std::to_string(scroll_offset + i + 1);

            // 确定显示文本：自定义输入项 + 光标停留时，显示输入 buffer + 光标块
            std::string display_text = item.label;
            ColorRole text_color = ColorRole::Default;
            if (item.is_custom_input && is_cursor && input_mode) {
                // 输入模式：显示 buffer + █ 光标块
                display_text = input_buffer + "\xe2\x96\x88";  // █
                text_color = ColorRole::Prompt;
            } else if (item.is_custom_input && !item.selected) {
                // 未选中的自定义项：Dim 色显示占位下划线
                text_color = ColorRole::Dim;
            }

            // 截断到终端宽度
            int available = width - LEFT_PADDING - 6;  // 6 = "[√] N、" 的显示宽度
            if (available > 0) {
                display_text = truncate_utf8_display(display_text, available);
            }

            // 行内容
            std::string line = std::string(LEFT_PADDING, ' ') + prefix + " " + num + "\xe3\x80\x81" + display_text;

            // 颜色：优先级 is_cursor+selected > is_cursor > selected > default
            ColorRole color = ColorRole::Default;
            if (is_cursor && item.selected) {
                color = ColorRole::SelectChecked;
            } else if (is_cursor) {
                color = (item.is_custom_input && input_mode) ? text_color : ColorRole::SelectCursor;
            } else if (item.selected) {
                color = ColorRole::SelectChecked;
            } else {
                color = text_color;
            }

            // 光标行加 ❯ 标识（替换前导空格）
            if (is_cursor) {
                line = std::string(LEFT_PADDING - 2, ' ') + "\xe2\x9d\xaf " + prefix + " " + num + "\xe3\x80\x81" + display_text;
            }

            scr->write(row, 0, line, color);
            ++row;
        }

        // 4. 滚动提示
        if (static_cast<int>(cur_tab.items.size()) > max_items) {
            std::string hint = std::string(LEFT_PADDING, ' ') +
                               std::format("({}/{})", cur_tab.cursor + 1, cur_tab.items.size());
            scr->write(row, 0, hint, ColorRole::Dim);
            ++row;
        }

        // 5. 底部提示行
        {
            int footer_row = overlay_bottom - 1;
            std::string footer;
            if (input_mode) {
                footer = std::string(LEFT_PADDING, ' ') +
                         "Enter \xe7\xa1\xae\xe8\xae\xa4\xe8\xbe\x93\xe5\x85\xa5  "  // Enter 确认输入
                         "Esc \xe5\x8f\x96\xe6\xb6\x88\xe8\xbe\x93\xe5\x85\xa5";       // Esc 取消输入
            } else {
                std::string mode_text = cur_tab.multi ? "\xe5\xa4\x9a\xe9\x80\x89" : "\xe5\x8d\x95\xe9\x80\x89";  // 多选/单选
                footer = std::string(LEFT_PADDING, ' ') +
                         "\xe2\x86\x91\xe2\x86\x93 \xe5\xaf\xbc\xe8\x88\xaa  "  // ↑↓ 导航
                         "\xe2\x86\x90\xe2\x86\x92 \xe5\x88\x87\xe6\x8d\xa2 Tab  "  // ←→ 切换 Tab
                         "\xe7\xa9\xba\xe6\xa0\xbc \xe5\x8b\xbe\xe9\x80\x89(" + mode_text + ")  "  // 空格 勾选
                         "Enter \xe7\xa1\xae\xe8\xae\xa4  "  // Enter 确认
                         "Esc \xe5\x8f\x96\xe6\xb6\x88";     // Esc 取消
            }
            if (static_cast<int>(footer.size()) > width) {
                footer = truncate_utf8_display(footer, width);
            }
            scr->write(footer_row, 0, footer, ColorRole::Dim);
        }

        scr->flush();
    };

    // ===== 主交互循环 =====
    ChoiceResult result;
    while (true) {
        do_render();
        char32_t key = term->platform()->read_char();

        // 跨线程唤醒（AskUser 超时等）：检查 cancel_flag，若已置位则取消面板
        if (key == KEY_WAKE) {
            if (cancel_flag && cancel_flag->load(std::memory_order_acquire)) {
                result.submitted = false;
                term->end_overlay();
                scr->reset_buffers();
                return result;
            }
            // 非 cancel 唤醒（不应发生），忽略后重新渲染
            continue;
        }

        if (input_mode) {
            // ===== 输入模式：所有按键都给输入框 =====
            if (key == KEY_ENTER) {
                // 确认输入：将输入文本设为自定义项的 id 和 label，并选中
                if (!input_buffer.empty()) {
                    auto& tab = tabs[current_tab];
                    if (tab.cursor < static_cast<int>(tab.items.size())) {
                        auto& item = tab.items[tab.cursor];
                        if (item.is_custom_input) {
                            item.id = input_buffer;
                            item.label = input_buffer;
                            if (tab.multi) {
                                item.selected = true;
                            } else {
                                for (auto& it : tab.items) it.selected = false;
                                item.selected = true;
                            }
                        }
                    }
                }
                input_mode = false;
                input_buffer.clear();
            } else if (key == KEY_ESC) {
                // 取消输入，回到选择模式
                input_mode = false;
                input_buffer.clear();
            } else if (key == KEY_UP || key == KEY_DOWN) {
                // 上下键：先提交当前输入（如有），再移动光标
                if (!input_buffer.empty()) {
                    auto& tab = tabs[current_tab];
                    if (tab.cursor < static_cast<int>(tab.items.size())) {
                        auto& item = tab.items[tab.cursor];
                        if (item.is_custom_input) {
                            item.id = input_buffer;
                            item.label = input_buffer;
                            if (tab.multi) {
                                item.selected = true;
                            } else {
                                for (auto& it : tab.items) it.selected = false;
                                item.selected = true;
                            }
                        }
                    }
                }
                input_mode = false;
                input_buffer.clear();
                // 移动光标
                auto& tab = tabs[current_tab];
                if (key == KEY_UP && tab.cursor > 0) tab.cursor--;
                else if (key == KEY_DOWN && tab.cursor < static_cast<int>(tab.items.size()) - 1) tab.cursor++;
                // 移动后若仍在自定义项上，自动进入输入模式
                if (tab.cursor < static_cast<int>(tab.items.size()) && tab.items[tab.cursor].is_custom_input) {
                    input_mode = true;
                }
            } else if (key == KEY_LEFT || key == KEY_RIGHT) {
                // 左右键：先提交当前输入（如有），再切换 Tab
                if (!input_buffer.empty()) {
                    auto& tab = tabs[current_tab];
                    if (tab.cursor < static_cast<int>(tab.items.size())) {
                        auto& item = tab.items[tab.cursor];
                        if (item.is_custom_input) {
                            item.id = input_buffer;
                            item.label = input_buffer;
                            if (tab.multi) {
                                item.selected = true;
                            } else {
                                for (auto& it : tab.items) it.selected = false;
                                item.selected = true;
                            }
                        }
                    }
                }
                input_mode = false;
                input_buffer.clear();
                // 切换 Tab
                if (key == KEY_LEFT) {
                    current_tab = (current_tab > 0) ? current_tab - 1 : static_cast<int>(tabs.size()) - 1;
                } else {
                    current_tab = (current_tab < static_cast<int>(tabs.size()) - 1) ? current_tab + 1 : 0;
                }
                scroll_offset = 0;
                // 切换后若光标在自定义项上，自动进入输入模式
                auto& new_tab = tabs[current_tab];
                if (new_tab.cursor < static_cast<int>(new_tab.items.size()) && new_tab.items[new_tab.cursor].is_custom_input) {
                    input_mode = true;
                }
            } else if (key == KEY_BACKSPACE || key == KEY_DELETE) {
                if (!input_buffer.empty()) {
                    // UTF-8 安全删除最后一个码点
                    size_t pos = input_buffer.size() - 1;
                    while (pos > 0 && (static_cast<unsigned char>(input_buffer[pos]) & 0xC0) == 0x80) {
                        --pos;
                    }
                    input_buffer.erase(pos);
                }
            } else if (key >= 0x20 && key != KEY_CTRL_C) {
                // 可打印字符：追加为 UTF-8
                if (key < 0x80) {
                    input_buffer += static_cast<char>(key);
                } else if (key < 0x800) {
                    input_buffer += static_cast<char>(0xC0 | (key >> 6));
                    input_buffer += static_cast<char>(0x80 | (key & 0x3F));
                } else if (key < 0x10000) {
                    input_buffer += static_cast<char>(0xE0 | (key >> 12));
                    input_buffer += static_cast<char>(0x80 | ((key >> 6) & 0x3F));
                    input_buffer += static_cast<char>(0x80 | (key & 0x3F));
                } else {
                    input_buffer += static_cast<char>(0xF0 | (key >> 18));
                    input_buffer += static_cast<char>(0x80 | ((key >> 12) & 0x3F));
                    input_buffer += static_cast<char>(0x80 | ((key >> 6) & 0x3F));
                    input_buffer += static_cast<char>(0x80 | (key & 0x3F));
                }
            }
            // 其他特殊键在输入模式下忽略
            continue;
        }

        // ===== 选择模式 =====
        if (key == KEY_UP) {
            auto& tab = tabs[current_tab];
            if (tab.cursor > 0) tab.cursor--;
            // 移动后若在自定义项上，自动进入输入模式（保留已有输入）
            if (tab.cursor < static_cast<int>(tab.items.size()) && tab.items[tab.cursor].is_custom_input) {
                input_mode = true;
            }
        } else if (key == KEY_DOWN) {
            auto& tab = tabs[current_tab];
            if (tab.cursor < static_cast<int>(tab.items.size()) - 1) tab.cursor++;
            // 移动后若在自定义项上，自动进入输入模式
            if (tab.cursor < static_cast<int>(tab.items.size()) && tab.items[tab.cursor].is_custom_input) {
                input_mode = true;
            }
        } else if (key == KEY_LEFT) {
            if (current_tab > 0) {
                current_tab--;
            } else {
                current_tab = static_cast<int>(tabs.size()) - 1;
            }
            scroll_offset = 0;
            // 切换后若光标在自定义项上，自动进入输入模式
            auto& new_tab = tabs[current_tab];
            if (new_tab.cursor < static_cast<int>(new_tab.items.size()) && new_tab.items[new_tab.cursor].is_custom_input) {
                input_mode = true;
            }
        } else if (key == KEY_RIGHT) {
            if (current_tab < static_cast<int>(tabs.size()) - 1) {
                current_tab++;
            } else {
                current_tab = 0;
            }
            scroll_offset = 0;
            // 切换后若光标在自定义项上，自动进入输入模式
            auto& new_tab = tabs[current_tab];
            if (new_tab.cursor < static_cast<int>(new_tab.items.size()) && new_tab.items[new_tab.cursor].is_custom_input) {
                input_mode = true;
            }
        } else if (key == KEY_SPACE) {
            auto& tab = tabs[current_tab];
            if (tab.cursor < static_cast<int>(tab.items.size())) {
                auto& item = tab.items[tab.cursor];
                if (item.is_custom_input) {
                    // 自定义输入项：进入输入模式（已自动进入，这里保留为兜底）
                    input_mode = true;
                } else if (tab.multi) {
                    item.selected = !item.selected;
                } else {
                    // 单选互斥：取消其他，选中当前
                    for (auto& it : tab.items) it.selected = false;
                    item.selected = true;
                }
            }
        } else if (key == KEY_ENTER) {
            if (current_tab < static_cast<int>(tabs.size()) - 1) {
                // 跳到下一个 Tab
                current_tab++;
                scroll_offset = 0;
                // 切换后若光标在自定义项上，自动进入输入模式
                auto& new_tab = tabs[current_tab];
                if (new_tab.cursor < static_cast<int>(new_tab.items.size()) && new_tab.items[new_tab.cursor].is_custom_input) {
                    input_mode = true;
                }
            } else {
                // 最后一个 Tab，提交
                result.submitted = true;
                for (const auto& tab : tabs) {
                    std::string answer;
                    bool first = true;
                    for (const auto& item : tab.items) {
                        if (item.selected) {
                            // 自定义输入项用 label（用户输入文本），普通项用 label
                            if (!first) answer += ",";
                            answer += item.label;
                            first = false;
                        }
                    }
                    result.answers.emplace_back(tab.question, std::move(answer));
                }
                term->end_overlay();
                scr->reset_buffers();
                return result;
            }
        } else if (key == KEY_ESC || key == KEY_CTRL_C) {
            // 取消整个面板
            result.submitted = false;
            term->end_overlay();
            scr->reset_buffers();
            return result;
        }
        // 其他按键忽略
    }
}

} // namespace tui
