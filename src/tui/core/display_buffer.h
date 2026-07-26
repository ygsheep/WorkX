/**
 * @file display_buffer.h
 * @brief 聊天滚动区域物理行镜像
 * @details 通过 tee Terminal::write / set_color / reset_color 的输出，
 *          维护一个滑动窗口环形缓冲，记录最近 N 个物理行的自包含字节串
 *          （每行 = leading SGR + 文本）。供覆盖层面板做背景快照/恢复。
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace tui {

/**
 * @brief 聊天输出镜像缓冲
 *
 * feed() 接收 Terminal 内容路径的字节流（文本 + SGR + \n + \x1b[2J 等），
 * 按 UTF-8 显示宽度切分为物理行并计入环形缓冲。snapshot() 返回指定
 * 1-based 屏幕行范围的自包含字节串，可用于 end_overlay 时按行重发恢复。
 */
class DisplayBuffer {
public:
    explicit DisplayBuffer(int capacity);

    /// @brief 喂入字节流（来自 Terminal::write / set_color / reset_color）
    void feed(std::string_view bytes);

    /// @brief 设置终端宽度（影响自动折行计算）
    void set_width(int width);

    /// @brief 设置终端高度（影响滚动区域行数计算）
    void set_height(int height);

    /// @brief 快照指定 1-based 屏幕行范围
    /// @return 每行一个自包含字节串（含 leading SGR），未填充行返回 ""
    std::vector<std::string> snapshot(int top_row, int bottom_row) const;

    /// @brief 清除所有行（处理 \x1b[2J）
    void clear_all();

    /// @brief 当前缓冲区已记录的物理行总数
    int row_count() const { return m_total_rows; }

private:
    /// @brief 终结当前物理行，推入环形缓冲
    void finalize_row();

    /// @brief 处理一段纯文本（不含转义），按宽度推进并折行
    void handle_text(std::string_view text);

    /// @brief 处理一个完整 CSI 序列（含前导 ESC[ 和终止符）
    void handle_csi(std::string_view params, char final_byte);

    /// @brief 刷新挂起的纯文本累积
    void flush_text_run();

    int m_capacity;
    int m_width = 80;
    int m_height = 24;
    int m_total_rows = 0;
    int m_col = 0;

    std::string m_active_sgr;   ///< 当前生效的 SGR（重发可重建当前颜色）
    std::string m_row_sgr;      ///< 当前行起始时的 leading SGR
    std::string m_row_builder;  ///< 当前行累积字节（含内联 SGR 变更）

    std::vector<std::string> m_rows;  ///< 环形缓冲
    int m_head = 0;                   ///< 下一个写入位置

    /// @brief feed 解析状态（跨调用保持转义序列上下文）
    enum class ParseState {
        Normal,
        Esc,    ///< 已读 \x1b
        Csi,    ///< 已读 \x1b[ ，累积参数
    } m_parse_state = ParseState::Normal;
    std::string m_csi_params;          ///< CSI 参数字节累积
    std::string m_text_run;            ///< 挂起的纯文本累积
};

} // namespace tui
