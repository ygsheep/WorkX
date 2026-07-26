/**
 * @file i_platform.h
 * @brief 终端平台抽象接口
 * @details Strategy 模式隔离平台代码（Win32 / POSIX）
 * @version 1.0.0
 */

#pragma once

#include <string_view>
#include "core/utils/result.h"

namespace tui {

/**
 * @brief 终端平台接口
 */
class IPlatform {
public:
    virtual ~IPlatform() = default;

    /// @brief 启用 raw mode（关闭行缓冲和回显）
    virtual agent::Result<void, std::string> enable_raw_mode() = 0;

    /// @brief 恢复终端原始设置
    virtual void disable_raw_mode() = 0;

    /// @brief 读取一个字符（raw mode 下）
    /// @return Unicode code point，WEOF 表示流结束
    virtual char32_t read_char() = 0;

    /// @brief 输出文本
    virtual void write_output(std::string_view text) = 0;

    /// @brief 移动光标（相对偏移，正数右移，负数左移）
    virtual void move_cursor(int cols) = 0;

    /// @brief 清除光标到行尾
    virtual void clear_to_end_of_line() = 0;

    /// @brief 获取终端宽度（列数）
    virtual int get_terminal_width() = 0;

    /// @brief 获取终端高度（行数）
    virtual int get_terminal_height() = 0;

    /// @brief 刷新输出
    virtual void flush() = 0;

    /// @brief 在光标位置写入一个 UTF-8 codepoint 并返回实际显示宽度
    virtual int put_codepoint(const char* utf8_data, size_t length, int expected_width) = 0;
};

} // namespace tui
