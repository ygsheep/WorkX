/**
 * @file main.cpp
 * @brief Agent Tools 子代理卡片 TUI 示例（边框白条流动动画）
 * @details 边框风格对齐 example_provider_form 的圆角框（╭─ 标题 ─╮ / │ │ / ╰─╯）。
 *          - 空闲态：白色边框（ColorRole::StatusBar）
 *          - 运行态：绿色边框（ColorRole::Success）+ 白色条沿边框顺时针流动（80ms/帧）
 *          - 布局：每行 2 张卡片；↑↓ 选择 · Space 启动/停止 · q/Esc 退出
 * @usage   example_agent_card
 */

#include <atomic>
#include <chrono>
#include <deque>
#include <format>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "app/factory.h"
#include "core/config/config_manager.h"
#include "core/events/event_bus.h"
#include "core/task/task_manager.h"
#include "tui/core/platform/i_platform.h"
#include "tui/core/screen.h"
#include "tui/core/terminal.h"
#include "tui/utils/utf8_utils.h"

using namespace tui;
using namespace agent;

namespace {

// 特殊按键码（与 example_provider_form / session_picker 一致）
constexpr char32_t KEY_UP     = 0xE002;
constexpr char32_t KEY_DOWN   = 0xE003;
constexpr char32_t KEY_ESC    = 0x1B;
constexpr char32_t KEY_CTRL_C = 0xE009;
constexpr char32_t KEY_RESIZE = 0xE00B;  // 终端尺寸变更（SIGWINCH → platform 返回）

// 边框字符（与 example_provider_form 一致：圆角）
const std::string TL     = "╭";
const std::string TR     = "╮";
const std::string BL     = "╰";
const std::string BR     = "╯";
const std::string HORIZ  = "─";
const std::string VERT   = "│";
const std::string TLJ    = "├";   // 分隔线左 T 型接头
const std::string TRJ    = "┤";   // 分隔线右 T 型接头
const std::string BULLET = "●";
const std::string CHECK  = "√";   // 工具完成（注意：✓ U+2713 会被判为全角宽 2，导致错位）
const std::string CIRCLE = "○";   // 工具待执行

constexpr int FRAME_MS = 40;        ///< 流动动画帧间隔
constexpr int FLOW_STEP = 3;        ///< 每帧流动步进（格）
constexpr int MAX_TOOLS = 3;        ///< 每张卡片最多显示的工具行数
constexpr int MARGIN = 2;           ///< 屏幕左右边距
constexpr int GAP = 2;              ///< 卡片间距
constexpr int TOOL_DURATION_MS = 2000;  ///< mock：每个工具调用耗时

/// mock 工具数据
struct ToolSpec {
    std::string name;     ///< 工具名
    std::string desc;     ///< 描述（空闲时显示）
    std::string result;   ///< mock 调用结果摘要（完成后显示）
};

/// 子代理卡片数据
struct AgentCard {
    std::string name;
    bool running = false;                            ///< 运行中（绿条流动）
    int flow_t = 0;                                  ///< 流动相位（边框路径下标）
    std::chrono::steady_clock::time_point run_start; ///< 本次运行开始时刻
    std::vector<ToolSpec> tools;                     ///< mock 工具序列（调用一轮后停止）
};

/// 边框顺时针路径（含四角）：顶行 → 右列 → 底行 → 左列
std::vector<std::pair<int, int>> build_path(int w, int h) {
    std::vector<std::pair<int, int>> p;
    for (int c = 0; c < w; ++c) p.emplace_back(0, c);
    for (int r = 1; r < h - 1; ++r) p.emplace_back(r, w - 1);
    for (int c = w - 1; c >= 0; --c) p.emplace_back(h - 1, c);
    for (int r = h - 2; r >= 1; --r) p.emplace_back(r, 0);
    return p;
}

/// 截断字符串到指定显示宽度（同 example_provider_form）
std::string truncate_to_width(const std::string& s, int max_cols) {
    if (display_width(s) <= max_cols) return s;
    std::string out;
    int w = 0;
    for (size_t i = 0; i < s.size() && w < max_cols - 1;) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 1;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (i + len > s.size()) break;
        out += s.substr(i, len);
        w += display_width(s.substr(i, len));
        i += len;
    }
    out += "…";
    return out;
}

} // namespace

// ============================================================================
// 主函数
// ============================================================================

int main() {
    // ---- 初始化 Terminal + Screen（对齐 example_provider_form） ----
    Terminal terminal(&EventBus::instance(), &ConfigManager::instance(),
                      &TaskManager::instance(),
                      make_terminal_config(ConfigManager::instance()));
    auto init_result = terminal.initialize();
    if (init_result.isErr()) {
        std::cerr << "[agent_card] Terminal 初始化失败: " << init_result.error() << "\n";
        terminal.restore();
        return 1;
    }
    Screen screen(&terminal);
    screen.resize(terminal.get_terminal_width(), terminal.get_terminal_height());

    // ---- 演示卡片（mock 子 Agent：按序调用真实工具名，一轮后停止） ----
    // 工具名取自 src/agent/tool 各工具的 name()（Read/Write/Edit/Glob/Grep/Bash/...）
    std::vector<AgentCard> cards = {
        {"Agent Tools", false, 0, {}, {
            {"Read", "读取文件", "已读取 256 行"},
            {"Grep", "正则搜索", "命中 8 处"},
            {"Glob", "文件名匹配", "匹配 5 个文件"},
        }},
        {"Code Review", false, 0, {}, {
            {"Read", "读取变更", "已读取 3 个文件"},
            {"Grep", "扫描告警", "发现 2 个警告"},
            {"Edit", "精确替换", "已应用 5 处修复"},
        }},
        {"Release Plan", false, 0, {}, {
            {"Bash", "执行命令", "退出码 0"},
            {"WebFetch", "抓取网页", "已转 Markdown"},
            {"Write", "写入文件", "已写入 CHANGELOG"},
        }},
    };

    int overlay_bottom = terminal.get_terminal_height() - 1;
    if (overlay_bottom < 1) overlay_bottom = 1;
    terminal.begin_overlay(1, overlay_bottom);

    int sel = 0;
    bool quit = false;
    std::atomic<bool> pump_running{true};

    // ---- 输入泵线程：阻塞读键 → 入队（主线程帧循环非阻塞消费，Screen 单线程访问） ----
    std::deque<char32_t> key_queue;
    std::mutex key_mutex;
    std::thread input_pump([&]() {
        auto* platform = terminal.platform();
        while (pump_running) {
            char32_t key = platform->read_char();
            std::lock_guard<std::mutex> lock(key_mutex);
            key_queue.push_back(key);
        }
    });

    // ---- 布局：每行 2 张卡片（终端 resize 时重算，对齐 choice_panel 模式） ----
    int card_w = 0;                                 // 卡片宽度（含边框）
    int card_h = 2 + MAX_TOOLS + 3;                 // 顶边 + 标题行 + 分隔线 + 工具行 + 状态行 + 底边
    int P = 0;                                      // 边框路径总长（恒偶数）
    int FLOW_LEN = 0;                               // 流动段长度：半个周长
    std::vector<int> path_index;                    // 边框格子 → 路径下标（-1 = 非边框）

    auto recompute_layout = [&]() {
        int w = screen.width();
        if (w < 60) w = 60;
        card_w = (w - 2 * MARGIN - GAP) / 2;
        auto path = build_path(card_w, card_h);
        P = static_cast<int>(path.size());
        FLOW_LEN = P / 2;
        path_index.assign(static_cast<size_t>(card_w * card_h), -1);
        for (int i = 0; i < P; ++i) {
            path_index[path[i].first * card_w + path[i].second] = i;
        }
    };
    recompute_layout();

    // ==================== 渲染 ====================
    auto do_render = [&]() {
        screen.clear();
        int row = 0;

        screen.write(row, MARGIN, "Agent Tools 子代理 · 卡片演示", ColorRole::StatusBar);
        row += 2;

        auto draw_card = [&](int top, int col, const AgentCard& card, bool selected) {
            // 边框底色恒为白色；运行中流动段为绿色（半周长）
            std::vector<bool> bright(P, false);
            if (card.running) {
                for (int k = 0; k < FLOW_LEN; ++k) {
                    bright[(card.flow_t - k + P) % P] = true;
                }
            }

            auto cell_color = [&](int rel_r, int rel_c) -> ColorRole {
                int pi = path_index[rel_r * card_w + rel_c];
                return (pi >= 0 && bright[pi]) ? ColorRole::Success : ColorRole::StatusBar;
            };

            // 按 UTF-8 字符逐格写（完整字符 + 逐格颜色）
            auto write_cells = [&](int rel_r, int col0, const std::string& text) {
                auto cells = decode_utf8_cells(text);
                int c = col0;
                for (const auto& uc : cells) {
                    if (c >= col0 + card_w) break;
                    screen.write(top + rel_r, c, uc.bytes, cell_color(rel_r, c - col0));
                    c += uc.width;
                }
            };

            // ---- 顶边（纯边框，标题在卡片内部）----
            std::string top_line = TL;
            for (int j = 0; j < card_w - 2; ++j) top_line += HORIZ;
            top_line += TR;
            write_cells(0, col, top_line);

            // ---- 标题行（卡片内部首行）----
            int tr = top + 1;
            std::string title = std::string("  ") + (selected ? "» " : "  ") + card.name;
            title = truncate_to_width(title, card_w - 2);
            int title_fill = (card_w - 2) - display_width(title);
            if (title_fill < 0) title_fill = 0;
            title += std::string(static_cast<size_t>(title_fill), ' ');
            screen.write(tr, col + 1, title,
                         card.running ? ColorRole::Success : ColorRole::StatusBar);
            screen.write(tr, col, VERT, cell_color(1, 0));
            screen.write(tr, col + card_w - 1, VERT, cell_color(1, card_w - 1));

            // ---- 分隔线（标题与内容分隔，白色）----
            std::string sep = TLJ;
            for (int j = 0; j < card_w - 2; ++j) sep += HORIZ;
            sep += TRJ;
            screen.write(top + 2, col, sep, ColorRole::StatusBar);

            // ---- 工具行（mock 三态：待执行 ○ / 调用中 ● / 完成 ✓）----
            int cur_tool = -1;  // 当前调用中的工具下标（仅运行中有效）
            int tool_ms = 0;    // 当前工具已耗时（ms）
            if (card.running) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - card.run_start).count();
                cur_tool = static_cast<int>(elapsed / TOOL_DURATION_MS);
                if (cur_tool >= static_cast<int>(card.tools.size())) {
                    cur_tool = static_cast<int>(card.tools.size()) - 1;
                }
                tool_ms = static_cast<int>(elapsed % TOOL_DURATION_MS);
            }
            for (int t = 0; t < MAX_TOOLS; ++t) {
                int r = top + 3 + t;
                screen.write(r, col, VERT, cell_color(t + 3, 0));
                if (t < static_cast<int>(card.tools.size())) {
                    const auto& tool = card.tools[t];
                    std::string line;
                    ColorRole line_color;
                    if (!card.running) {
                        line = "  " + BULLET + " " + tool.name + "  " + tool.desc;
                        line_color = ColorRole::Default;
                    } else if (t < cur_tool) {
                        line = "  " + CHECK + " " + tool.name + "  " + tool.result;
                        line_color = ColorRole::Success;
                    } else if (t == cur_tool) {
                        line = std::format("  {} {} 调用中 {:.1f}s",
                                           BULLET, tool.name, tool_ms / 1000.0);
                        line_color = ColorRole::ThinkingIndicator;
                    } else {
                        line = "  " + CIRCLE + " " + tool.name + "  " + tool.desc;
                        line_color = ColorRole::Dim;
                    }
                    line = truncate_to_width(line, card_w - 4);
                    int fill = (card_w - 2) - display_width(line);
                    if (fill < 0) fill = 0;
                    line += std::string(static_cast<size_t>(fill), ' ');
                    screen.write(r, col + 1, line, line_color);
                }
                screen.write(r, col + card_w - 1, VERT, cell_color(t + 3, card_w - 1));
            }

            // ---- 状态行 ----
            int sr = top + 3 + MAX_TOOLS;
            std::string status;
            ColorRole status_color;
            if (card.running) {
                auto secs = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - card.run_start).count();
                int done = cur_tool;
                status = std::format("{} 运行中 {}s · 工具 {}/{}",
                                     BULLET, secs, done + 1, card.tools.size());
                status_color = ColorRole::Success;
            } else {
                status = "空闲 · Space 启动";
                status_color = ColorRole::Dim;
            }
            std::string status_line = "  " + status;
            status_line = truncate_to_width(status_line, card_w - 4);
            int fill = (card_w - 2) - display_width(status_line);
            if (fill < 0) fill = 0;
            status_line += std::string(static_cast<size_t>(fill), ' ');
            screen.write(sr, col + 1, status_line, status_color);
            screen.write(sr, col, VERT, cell_color(MAX_TOOLS + 3, 0));
            screen.write(sr, col + card_w - 1, VERT, cell_color(MAX_TOOLS + 3, card_w - 1));

            // ---- 底边 ----
            std::string bottom = BL;
            for (int j = 0; j < card_w - 2; ++j) bottom += HORIZ;
            bottom += BR;
            write_cells(card_h - 1, col, bottom);
        };

        // 每行 2 张卡片
        for (int i = 0; i < static_cast<int>(cards.size()); ++i) {
            int col = MARGIN + (i % 2) * (card_w + GAP);
            int top = row + (i / 2) * (card_h + 1);
            draw_card(top, col, cards[i], i == sel);
        }

        // 底部提示
        int hint_row = row + ((static_cast<int>(cards.size()) + 1) / 2) * (card_h + 1);
        screen.write(hint_row, MARGIN, "↑↓ 选择卡片 · Space 启动/停止 · q/Esc 退出", ColorRole::Dim);

        screen.flush();
    };

    // ==================== 帧循环 ====================
    while (!quit) {
        do_render();

        // 消费按键（非阻塞）
        char32_t key = 0;
        {
            std::lock_guard<std::mutex> lock(key_mutex);
            if (!key_queue.empty()) {
                key = key_queue.front();
                key_queue.pop_front();
            }
        }

        const int n = static_cast<int>(cards.size());
        if (key == KEY_UP) {
            sel = (sel <= 0) ? n - 1 : sel - 1;
        } else if (key == KEY_DOWN) {
            sel = (sel >= n - 1) ? 0 : sel + 1;
        } else if (key == ' ') {
            auto& card = cards[sel];
            card.running = !card.running;
            if (card.running) card.run_start = std::chrono::steady_clock::now();
        } else if (key == KEY_RESIZE) {
            // 终端尺寸变更：重算布局并强制全量重绘（同 choice_panel/session_picker）
            int new_w = terminal.get_terminal_width();
            int new_h = terminal.get_terminal_height();
            if (new_w != screen.width() || new_h != screen.height()) {
                screen.resize(new_w, new_h);
                screen.clear_terminal();  // 清物理屏 + 重置双缓冲 → 全量重绘
                recompute_layout();
            }
        } else if (key == 'q' || key == KEY_ESC || key == KEY_CTRL_C) {
            quit = true;
        }

        // 推进流动相位 + mock 调用（一轮工具全部完成 → 自动停止）
        for (auto& card : cards) {
            if (!card.running) continue;
            card.flow_t = (card.flow_t + FLOW_STEP) % P;
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - card.run_start).count();
            if (elapsed >= static_cast<int>(card.tools.size()) * TOOL_DURATION_MS) {
                card.running = false;
            }
        }

        // 帧间隔节流
        std::this_thread::sleep_for(std::chrono::milliseconds(FRAME_MS));
    }

    // ---- 退出清理 ----
    pump_running = false;
    terminal.platform()->notify_wake();  // 唤醒阻塞的 read_char，让泵线程退出
    input_pump.join();
    terminal.end_overlay();
    screen.reset_buffers();
    terminal.restore();
    return 0;
}