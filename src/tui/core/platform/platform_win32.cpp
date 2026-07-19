/**
 * @file platform_win32.cpp
 * @brief Win32 Console API 平台实现
 * @version 1.0.0
 */

#include "tui/core/platform/i_platform.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <memory>

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

namespace agent {

// Win32 特殊键码（Private Use Area，不与真实输入冲突）
static constexpr char32_t KEY_ARROW_LEFT       = 0xE000;
static constexpr char32_t KEY_ARROW_RIGHT      = 0xE001;
static constexpr char32_t KEY_ARROW_UP         = 0xE002;
static constexpr char32_t KEY_ARROW_DOWN       = 0xE003;
static constexpr char32_t KEY_HOME             = 0xE004;
static constexpr char32_t KEY_END              = 0xE005;
static constexpr char32_t KEY_CTRL_ARROW_LEFT  = 0xE006;
static constexpr char32_t KEY_CTRL_ARROW_RIGHT = 0xE007;
static constexpr char32_t KEY_DELETE           = 0xE008;
static constexpr char32_t KEY_CTRL_C           = 0xE009;
static constexpr char32_t KEY_CTRL_O           = 0xE00A;

class Win32Platform : public IPlatform {
public:
    Result<void, std::string> enable_raw_mode() override {
        m_h_output = GetStdHandle(STD_OUTPUT_HANDLE);
        if (m_h_output == INVALID_HANDLE_VALUE || !GetConsoleMode(m_h_output, &m_original_output_mode)) {
            m_h_output = GetStdHandle(STD_ERROR_HANDLE);
            if (m_h_output == INVALID_HANDLE_VALUE || !GetConsoleMode(m_h_output, &m_original_output_mode)) {
                m_h_output = nullptr;
                return Result<void, std::string>::err("Failed to get console output handle");
            }
        }

        // 启用 VT 处理
        if (!(m_original_output_mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
            SetConsoleMode(m_h_output, m_original_output_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }

        SetConsoleOutputCP(CP_UTF8);

        m_h_input = GetStdHandle(STD_INPUT_HANDLE);
        if (m_h_input != INVALID_HANDLE_VALUE && GetConsoleMode(m_h_input, &m_original_input_mode)) {
            _setmode(_fileno(stdin), _O_WTEXT);
            DWORD new_mode = m_original_input_mode;
            new_mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
            SetConsoleMode(m_h_input, new_mode);
        }

        m_raw_mode_enabled = true;
        return Result<void, std::string>::ok();
    }

    void disable_raw_mode() override {
        if (!m_raw_mode_enabled) return;

        if (m_h_output) {
            SetConsoleMode(m_h_output, m_original_output_mode);
        }
        if (m_h_input && m_h_input != INVALID_HANDLE_VALUE) {
            SetConsoleMode(m_h_input, m_original_input_mode);
            _setmode(_fileno(stdin), _O_U8TEXT);
        }

        m_raw_mode_enabled = false;
    }

    char32_t read_char() override {
        wchar_t high_surrogate = 0;

        while (true) {
            INPUT_RECORD record;
            DWORD count = 0;
            if (!ReadConsoleInputW(m_h_input, &record, 1, &count) || count == 0) {
                return WEOF;
            }

            if (record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown) {
                wchar_t wc = record.Event.KeyEvent.uChar.UnicodeChar;
                const DWORD ctrl_mask = LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED;
                const bool ctrl_pressed = (record.Event.KeyEvent.dwControlKeyState & ctrl_mask) != 0;

                // Ctrl 字母组合键：UnicodeChar 可能非 0（如 Ctrl+O=0x0F, Ctrl+C=0x03），
                // 需在 wc==0 检查之前先拦截
                if (ctrl_pressed) {
                    switch (record.Event.KeyEvent.wVirtualKeyCode) {
                        case 'C':       return KEY_CTRL_C;
                        case 'O':       return KEY_CTRL_O;
                        default:        break;  // 其他 Ctrl 组合键走默认逻辑
                    }
                }

                if (wc == 0) {
                    switch (record.Event.KeyEvent.wVirtualKeyCode) {
                        case VK_LEFT:   return ctrl_pressed ? KEY_CTRL_ARROW_LEFT  : KEY_ARROW_LEFT;
                        case VK_RIGHT:  return ctrl_pressed ? KEY_CTRL_ARROW_RIGHT : KEY_ARROW_RIGHT;
                        case VK_UP:     return KEY_ARROW_UP;
                        case VK_DOWN:   return KEY_ARROW_DOWN;
                        case VK_HOME:   return KEY_HOME;
                        case VK_END:    return KEY_END;
                        case VK_DELETE: return KEY_DELETE;
                        default:        continue;
                    }
                }

                // 处理 UTF-16 代理对
                if ((wc >= 0xD800) && (wc <= 0xDBFF)) {
                    high_surrogate = wc;
                    continue;
                }
                if ((wc >= 0xDC00) && (wc <= 0xDFFF)) {
                    if (high_surrogate != 0) {
                        return ((high_surrogate - 0xD800) << 10) + (wc - 0xDC00) + 0x10000;
                    }
                }

                return static_cast<char32_t>(wc);
            }
        }
    }

    void write_output(std::string_view text) override {
        if (text.empty()) return;

        if (m_h_output) {
            DWORD written = 0;
            WriteConsoleA(m_h_output, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
        } else {
            // 非 console 模式回退到 stdout
            fwrite(text.data(), 1, text.size(), stdout);
            fflush(stdout);
        }
    }

    void move_cursor(int cols) override {
        if (cols == 0) return;

        // 统一使用 ANSI 转义序列移动光标，避免 Win32 Console API
        // 与 ANSI 序列的坐标系不一致问题（缓冲区坐标 vs 视窗坐标）
        char cmd[16];
        if (cols > 0) {
            snprintf(cmd, sizeof(cmd), "\x1b[%dC", cols);
        } else {
            snprintf(cmd, sizeof(cmd), "\x1b[%dD", -cols);
        }
        write_output(cmd);
    }

    void clear_to_end_of_line() override {
        write_output("\x1b[K");
    }

    int get_terminal_width() override {
        if (!m_h_output) return 80;
        CONSOLE_SCREEN_BUFFER_INFO info;
        if (!GetConsoleScreenBufferInfo(m_h_output, &info)) return 80;
        // 使用视窗宽度（srWindow），而非缓冲区宽度（dwSize.X）
        // 在 Windows Terminal 下，缓冲区宽度可能远大于视窗宽度
        return info.srWindow.Right - info.srWindow.Left + 1;
    }

    int get_terminal_height() override {
        if (!m_h_output) return 24;
        CONSOLE_SCREEN_BUFFER_INFO info;
        if (!GetConsoleScreenBufferInfo(m_h_output, &info)) return 24;
        return info.srWindow.Bottom - info.srWindow.Top + 1;
    }

    void flush() override {
        // FlushConsoleInputBuffer 清除输入缓冲区，不应在输出 flush 中调用
        // 仅刷新 stdout 即可
        fflush(stdout);
    }
    int put_codepoint(const char* utf8_data, size_t length, int expected_width) override {
        if (m_h_output) {
            CONSOLE_SCREEN_BUFFER_INFO info;
            if (!GetConsoleScreenBufferInfo(m_h_output, &info)) return expected_width;

            int viewport_width = info.srWindow.Right - info.srWindow.Left + 1;
            COORD initial_pos = info.dwCursorPosition;
            DWORD written = 0;
            WriteConsoleA(m_h_output, utf8_data, static_cast<DWORD>(length), &written, nullptr);

            CONSOLE_SCREEN_BUFFER_INFO new_info;
            GetConsoleScreenBufferInfo(m_h_output, &new_info);

            // 在视窗宽度的最右列写入字符时，控制台会自动换行
            // 此时需要用空格触发延迟换行并回退，让后续光标位置正确
            if (utf8_data[0] != 0x09 && initial_pos.X == viewport_width - 1) {
                WriteConsoleA(m_h_output, " \b", 2, &written, nullptr);
                GetConsoleScreenBufferInfo(m_h_output, &new_info);
            }

            int width = new_info.dwCursorPosition.X - initial_pos.X;
            if (width < 0) {
                width += info.dwSize.X;  // 换行时 X 回绕
            }
            return width;
        } else {
            fwrite(utf8_data, 1, length, stdout);
            fflush(stdout);
            return expected_width;
        }
    }

private:
    HANDLE m_h_output = nullptr;
    HANDLE m_h_input = nullptr;
    DWORD m_original_output_mode = 0;
    DWORD m_original_input_mode = 0;
    bool m_raw_mode_enabled = false;
};

std::unique_ptr<IPlatform> create_platform() {
    return std::make_unique<Win32Platform>();
}

} // namespace workx
