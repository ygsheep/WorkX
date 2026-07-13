/**
 * @file markdown_renderer.h
 * @brief Markdown 表格解析与渲染
 * @details 将 Markdown 表格语法转换为 Unicode box-drawing 字符画表格
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace agent {

/**
 * @brief 列对齐方式
 */
enum class TableAlign {
    Default,   ///< 无对齐标记
    Left,      ///< :---
    Center,    ///< :--:
    Right      ///< ---:
};

/**
 * @brief 解析后的 Markdown 表格
 */
struct MarkdownTable {
    std::vector<std::string> headers;               ///< 表头单元格（已 trim）
    std::vector<std::vector<std::string>> rows;      ///< 数据行
    std::vector<TableAlign> alignments;              ///< 每列对齐方式
    bool valid = false;                              ///< 解析是否成功
};

/**
 * @brief 判断一行是否像表格行（trim 后以 | 开头）
 */
bool is_table_row(std::string_view line);

/**
 * @brief 判断一行是否为有效的表格分隔行
 * @param line 待检测的行
 * @param alignments [out] 若有效，填充每列对齐方式
 * @return true 如果是有效分隔行
 */
bool is_table_separator(std::string_view line, std::vector<TableAlign>& alignments);

/**
 * @brief 拆分表格行为单元格
 * @details 去除首尾 |，按未转义的 | 分割，trim 每个单元格
 *          处理 \| 转义（替换为 |）
 * @param line 表格行，如 "| a | b |"
 * @return 单元格文本列表
 */
std::vector<std::string> split_table_row(std::string_view line);

/**
 * @brief 解析多行为 MarkdownTable
 * @details 第 0 行=表头，第 1 行=分隔行（需验证），第 2+ 行=数据行
 *          参差不齐的行补空字符串
 * @param lines 完整表格行（必须含表头+分隔行）
 * @return 解析结果，.valid=false 表示分隔行缺失或无效
 */
MarkdownTable parse_table(const std::vector<std::string>& lines);

/**
 * @brief 渲染 MarkdownTable 为 box-drawing 文本
 * @param table 已解析的表格（必须 valid）
 * @param max_width 最大显示宽度，0=不限宽
 * @return 多行字符串，含 ┌─┬─┐│├─┼─┤└─┴─┘ 边框
 */
std::string render_table(const MarkdownTable& table, int max_width = 0);

/**
 * @brief 表格流式缓冲状态机
 * @details 处理流式输入：检测表格起始、验证分隔行、缓冲数据行
 *          Terminal 无关，可独立单元测试
 */
class TableBuffer {
public:
    /**
     * @brief 输入一行文本
     * @return true=该行被消费（属于表格），false=该行不属于表格
     */
    bool feed_line(const std::string& line);

    /// 表格已结束且有效（等待渲染）
    bool is_complete() const { return m_state == State::Complete; }

    /// 分隔行缺失或无效（缓冲内容应按普通文本输出）
    bool is_invalid() const { return m_state == State::Invalid; }

    /// 是否正在缓冲表格
    bool is_active() const { return m_state == State::PendingSeparator || m_state == State::Collecting; }

    /// 获取缓冲的表格行
    const std::vector<std::string>& lines() const { return m_lines; }

    /// 重置状态
    void clear() { m_lines.clear(); m_state = State::Empty; }

private:
    enum class State {
        Empty,            ///< 未开始
        PendingSeparator,  ///< 已缓冲表头行，等待分隔行
        Collecting,       ///< 表格确认，正在收集数据行
        Complete,          ///< 表格结束，有效
        Invalid            ///< 分隔行缺失
    };
    State m_state = State::Empty;
    std::vector<std::string> m_lines;
};

// ============================================================================
// 行内渲染
// ============================================================================

/**
 * @brief 渲染行内 Markdown 语法
 * @details 处理 **粗体**, *斜体*, ***粗斜体***, ~~删除线~~, `行内代码`, 转义符
 */
std::string render_inline(std::string_view text);

// ============================================================================
// 块级渲染
// ============================================================================

/**
 * @brief 判断行是否为分隔线（--- / *** / ___）
 */
bool is_horizontal_rule(std::string_view line);

/**
 * @brief 判断行是否为列表项
 */
bool is_list_item(std::string_view line);

/**
 * @brief 渲染标题（H1~H6）
 * @param level 1~6
 * @param text 标题文本（不含 # 前缀）
 */
std::string render_heading(int level, std::string_view text);

/**
 * @brief 渲染分隔线
 * @param width 分隔线字符数
 */
std::string render_hr(int width = 60);

/**
 * @brief 渲染列表项
 */
std::string render_list_item(std::string_view line);

/**
 * @brief 渲染代码块
 * @param lang 语言标签（可空）
 * @param lines 代码行
 */
std::string render_code_block(std::string_view lang, const std::vector<std::string>& lines);

} // namespace workx
