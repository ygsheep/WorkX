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
#include <iostream>
#include <memory>

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

namespace tui {

using namespace agent;  // P0: tui→agent 类型引用过渡方案，后续 P2/P3 收紧到显式前缀

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
static constexpr char32_t KEY_RESIZE           = 0xE00B;  // 终端尺寸变更

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

        // 设置输出 CP 为 UTF-8，失败时记录但不阻塞（旧版 Windows 仍可工作，仅 UTF-8 显示乱码）
        if (!SetConsoleOutputCP(CP_UTF8)) {
            std::cerr << "Warning: SetConsoleOutputCP(CP_UTF8) failed, UTF-8 display may be incorrect\n";
        }

        m_h_input = GetStdHandle(STD_INPUT_HANDLE);
        if (m_h_input != INVALID_HANDLE_VALUE && GetConsoleMode(m_h_input, &m_original_input_mode)) {
            // stdin 统一用 UTF-8 模式（_O_U8TEXT），与 disable_raw_mode 一致；
            // 原来的 _O_WTEXT（UTF-16）与 WriteConsoleA 输出链路不对称，且让 CRT 函数困惑
            _setmode(_fileno(stdin), _O_U8TEXT);
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

            // 捕获终端 resize 事件（Windows Terminal 拖动边框/最大化时触发）
            if (record.EventType == WINDOW_BUFFER_SIZE_EVENT) {
                return KEY_RESIZE;
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
        // G.2：raw mode 启用时（即 VT 处理已启用），stdout 通过 WriteConsoleA 写入，
        //      fflush(stdout) 在 VT 模式下是 no-op（stdout 未与 Console 句柄关联的
        //      CRT 缓冲区），导致 printf 与 WriteConsoleA 混用时输出顺序错乱。
        //      改用 FlushFileBuffers 强制刷新 Console 句柄的内核缓冲区。
        if (m_raw_mode_enabled && m_h_output && m_h_output != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(m_h_output);
        } else {
            fflush(stdout);
        }
    }
    int put_codepoint(const char* utf8_data, size_t length, int expected_width) override {
        if (m_h_output) {
            CONSOLE_SCREEN_BUFFER_INFO info;
            if (!GetConsoleScreenBufferInfo(m_h_output, &info)) return expected_width;

            // G.1：initial_pos.X 是 SHORT（16 位），cast 到 int 后再与 viewport_width 比较
            // 避免在极端宽度（>32767，理论上不会发生但防御性编程）下 SHORT 溢出
            int viewport_width = info.srWindow.Right - info.srWindow.Left + 1;
            COORD initial_pos = info.dwCursorPosition;
            int initial_pos_x = static_cast<int>(initial_pos.X);
            DWORD written = 0;
            WriteConsoleA(m_h_output, utf8_data, static_cast<DWORD>(length), &written, nullptr);

            CONSOLE_SCREEN_BUFFER_INFO new_info;
            GetConsoleScreenBufferInfo(m_h_output, &new_info);

            // 在视窗宽度的最右列写入字符时，控制台会自动换行
            // 此时需要用空格触发延迟换行并回退，让后续光标位置正确
            // G.1：比较前 cast 到 int，并限制 max_x 上限为 SHORT 范围
            int max_x = std::min(viewport_width - 1, 32767);
            if (utf8_data[0] != 0x09 && initial_pos_x == max_x) {
                WriteConsoleA(m_h_output, " \b", 2, &written, nullptr);
                GetConsoleScreenBufferInfo(m_h_output, &new_info);
            }

            int new_pos_x = static_cast<int>(new_info.dwCursorPosition.X);
            int width = new_pos_x - initial_pos_x;
            if (width < 0) {
                width += static_cast<int>(info.dwSize.X);  // 换行时 X 回绕
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

} // namespace tui
