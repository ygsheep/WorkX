/**
 * @file screen.h
 * @brief 差分渲染屏幕缓冲
 * @details 管理虚拟屏幕缓冲区，flush 时只输出变化的行。
 *          支持与 Direct 模式（Terminal 直写）之间切换。
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <vector>
#include <memory>
#include "tui/core/color_scheme.h"

namespace workx {

class Terminal;

/// @brief 屏幕单元
struct ScreenCell {
    std::string ch = " ";               ///< 完整 UTF-8 字符（默认空格）
    ColorRole color = ColorRole::Default;  ///< 颜色
    int width = 1;                      ///< 显示宽度（1 或 2，0=宽字符延续）
};

/// @brief 屏幕行
struct ScreenLine {
    std::vector<ScreenCell> cells;
    bool dirty = true;             ///< 自上次 flush 后是否变更
};

/// @brief 差分渲染屏幕
/// @details 维护一个二维字符缓冲区。write/draw 修改缓冲区，
///          flush() 时与上一帧对比，只输出变化的行到终端。
///          切换回 Direct 模式时调用 clear() + flush() 清空虚拟屏幕。
class Screen {
public:
    explicit Screen(Terminal* terminal);

    /// @brief 在指定位置写入文本
    /// @param row 行号（0-based）
    /// @param col 列号（0-based）
    /// @param text 文本内容
    /// @param color 颜色
    void write(int row, int col, const std::string& text,
               ColorRole color = ColorRole::Default);

    /// @brief 填充区域
    void fill(int row, int col, int width, char c);

    /// @brief 绘制边框盒子
    /// @param row 起始行
    /// @param col 起始列
    /// @param width 盒子宽度（含边框）
    /// @param title 标题文本
    void draw_box(int row, int col, int width, const std::string& title);

    /// @brief 清空虚拟缓冲区，标记全屏脏
    void clear();

    /// @brief 清空物理终端并重置缓冲区（用于切换回 Direct 模式）
    /// @details 发送 \x1b[2J\x1b[H 到终端，同时重置内部状态，
    ///          避免切换渲染模式时的内容残留
    void clear_terminal();

    /// @brief 重置内部缓冲区（m_lines/m_previous 全部置空格），不发送任何终端输出
    /// @details 用于 end_overlay 恢复终端后，清除 Screen 的陈旧 diff 状态，
    ///          避免下次 flush 写出残留内容
    void reset_buffers();

    /// @brief 调整缓冲区大小
    void resize(int w, int h);

    /// @brief 对比前帧，输出差异到终端
    void flush();

    /// @brief 获取缓冲区宽度
    int width() const { return m_width; }
    /// @brief 获取缓冲区高度
    int height() const { return m_height; }

private:
    /// @brief 确保缓冲区至少有指定行数
    void ensure_lines(int row);

    /// @brief 输出单行到终端（带 ANSI 定位和颜色）
    void render_line(int row, const ScreenLine& line);

    Terminal* m_terminal;
    int m_width = 0;
    int m_height = 0;
    std::vector<ScreenLine> m_lines;      // 当前帧
    std::vector<ScreenLine> m_previous;   // 上一帧
};

} // namespace workx
