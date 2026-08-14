/**
 * @file line_editor.h
 * @brief 行编辑器
 * @details UTF-8/CJK 宽度计算、历史、基础编辑操作、Tab 补全回调
 * @version 1.0.0
 */

#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>

namespace tui {

class IPlatform;

/**
 * @brief Tab 补全候选
 */
struct CompletionCandidate {
    std::string text;       ///< 替换后的完整行
    size_t cursor_pos;      ///< 替换后光标字节位置
};

/**
 * @brief 行编辑器
 */
class LineEditor {
public:
    using CompletionCallback = std::function<std::vector<CompletionCandidate>(
        std::string_view line, size_t byte_pos)>;

    /// @brief 命令面板导航回调（↑↓ 键拦截）
    /// @param key 按键码
    /// @return true 如果已处理（不应继续传递给 history）
    using CommandNavCallback = std::function<bool(char32_t key)>;

    /// @brief 命令面板 Tab 补全回调
    /// @return 补全文本，空字符串表示未处理
    using CommandTabCallback = std::function<std::string()>;

    /// @brief 输入变化通知回调（用于更新命令面板过滤）
    using InputChangedCallback = std::function<void(const std::string& line)>;

    /// @brief #45：权限模式切换回调（Shift+Tab 键拦截）
    /// @details Shift+Tab（Backtab）按下时调用，宿主据此切换会话权限模式。
    using PermToggleCallback = std::function<void()>;

    /// @brief 光标离开输出区通知（定位到输入行时调用）
    using CursorLeftOutputCallback = std::function<void()>;

    /// @brief 编辑状态变化通知（read_line 开始/结束时调用）
    /// @param editing true=开始编辑，false=结束编辑
    using EditingChangedCallback = std::function<void(bool editing)>;

    /// @brief 终端尺寸变更通知（read_char 收到 KEY_RESIZE 时调用）
    /// @details Terminal 在此回调中刷新 scroll region、重放 DisplayBuffer、
    ///          发布 TerminalResizeEvent；回调返回后 LineEditor 重新定位输入行。
    using ResizeCallback = std::function<void()>;

    /// @brief 输入区行数变化通知（编辑多行文本时调整滚动区保护范围）
    /// @param lines 当前输入区占用的终端行数（>= 1）
    using EditLinesCallback = std::function<void(int lines)>;

    explicit LineEditor(IPlatform* platform);

    /// @brief 设置 Tab 补全回调
    void set_completion_callback(CompletionCallback cb);

    /// @brief 设置命令面板导航回调（↑↓ 键拦截）
    void set_command_nav_callback(CommandNavCallback cb);

    /// @brief #45：设置权限模式切换回调（Shift+Tab 键拦截）
    void set_perm_toggle_callback(PermToggleCallback cb);

    /// @brief 设置命令面板 Tab 补全回调
    void set_command_tab_callback(CommandTabCallback cb);

    /// @brief 设置输入变化通知回调
    void set_input_changed_callback(InputChangedCallback cb);

    /// @brief 设置光标离开输出区通知回调
    void set_cursor_left_output_callback(CursorLeftOutputCallback cb);

    /// @brief 设置编辑状态变化通知回调
    void set_editing_changed_callback(EditingChangedCallback cb);

    /// @brief 设置终端尺寸变更回调
    void set_resize_callback(ResizeCallback cb);

    /// @brief 设置输入区行数变化回调
    void set_edit_lines_callback(EditLinesCallback cb);

    /// @brief 批量加载历史条目（追加到现有历史）
    void load_history(const std::vector<std::string>& entries);

    /// @brief 获取当前历史列表（用于持久化）
    const std::vector<std::string>& get_history() const { return m_history; }

    /// @brief 编辑一行输入（阻塞直到用户按 Enter）
    /// @return 用户输入的行（不含尾部换行），空字符串+stream_end=true 表示 EOF
    struct ReadResult {
        std::string text;
        bool stream_end = false;
        bool is_command = false;  ///< 以 / 开头的命令
        bool interrupted = false; ///< Ctrl+C 中断
        bool escape = false;      ///< 独立 Esc 键（等同打断，与 Ctrl+C 语义一致）
        bool ctrl_o = false;      ///< Ctrl+O 按下（切换思考视图）
        bool woken_by_ask = false; ///< 被跨线程唤醒（如 AskUser 请求），主循环需检查 pending 事件
    };
    ReadResult read_line(const std::string& prompt);

    /// @brief 获取历史条目数
    size_t get_history_size() const { return m_history.size(); }

private:
    // UTF-8 辅助
    static char32_t decode_utf8(const std::string& input, size_t pos, size_t& advance);
    static void append_utf8(char32_t ch, std::string& out);
    static size_t prev_utf8_char_pos(const std::string& line, size_t pos);
    static size_t next_utf8_char_pos(const std::string& line, size_t pos);
    static int estimate_width(char32_t codepoint);
    static bool is_space_codepoint(char32_t cp);

    // 编辑操作
    void delete_at_cursor();
    void set_line_contents(const std::string& new_line, int cursor_byte_pos = -1);
    void move_to_line_start();
    void move_to_line_end();
    void move_word_left();
    void move_word_right();
    void move_cursor_to(size_t char_pos);

    // 多行支持
    size_t line_count() const;            ///< 文本行数（\n 数 + 1）
    size_t cur_line_idx() const;          ///< 光标所在行索引（0-based）
    size_t line_start_char(size_t line_idx) const;      ///< 行首字符索引
    size_t line_char_count(size_t line_idx) const;      ///< 行内字符数（不含行尾 \n）
    size_t char_to_byte(size_t char_pos) const;         ///< 字符索引 → 字节索引
    char32_t char_at(size_t char_pos) const;            ///< 第 char_pos 个字符的码点
    std::string line_text(size_t line_idx) const;       ///< 行文本（不含行尾 \n）
    int line_prefix_width(size_t line_idx, size_t char_in_line) const;  ///< 行内前缀宽度
    int input_area_max_lines() const;     ///< 输入区行数上限（终端高度 - 3）
    void redraw_input();                  ///< 全量重绘输入区并定位光标

    // 历史
    void history_prev();
    void history_next();

    IPlatform* m_platform;

    // 编辑状态
    std::string m_line;
    std::vector<int> m_widths;   ///< 每个字符的显示宽度
    size_t m_char_pos = 0;       ///< 字符位置（code point 索引）
    size_t m_byte_pos = 0;       ///< 字节位置
    std::string m_prompt;        ///< 当前提示符（redraw 使用）
    bool m_is_continuation = false;  ///< 续行模式标记
    size_t m_last_win_lines = 1; ///< 上一次渲染的输入区行数（行数减少时用于清除残留）

    // 历史状态
    std::vector<std::string> m_history;
    size_t m_history_idx = SIZE_MAX;
    std::string m_backup_line;   ///< 进入历史浏览前的当前行

    CompletionCallback m_completion_cb;
    CommandNavCallback m_command_nav_cb;
    CommandTabCallback m_command_tab_cb;
    InputChangedCallback m_input_changed_cb;
    PermToggleCallback m_perm_toggle_cb;  ///< #45：Shift+Tab 权限模式切换回调
    CursorLeftOutputCallback m_cursor_left_output_cb;
    EditingChangedCallback m_editing_changed_cb;
    ResizeCallback m_resize_cb;
    EditLinesCallback m_edit_lines_cb;
};

} // namespace tui
