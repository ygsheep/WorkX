/**
 * @file platform_posix.cpp
 * @brief POSIX (Linux/macOS) termios 平台实现
 * @version 1.0.0
 */

#include "tui/core/platform/i_platform.h"

#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>

namespace agent {

// POSIX 特殊键码
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

class PosixPlatform : public IPlatform {
public:
    Result<void, std::string> enable_raw_mode() override {
        if (tcgetattr(STDIN_FILENO, &m_original) != 0) {
            return Result<void, std::string>::err("Failed to get terminal attributes");
        }

        struct termios raw = m_original;
        raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
        raw.c_oflag &= ~(OPOST);
        raw.c_cflag |= (CS8);
        raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
        raw.c_cc[VMIN] = 1;
        raw.c_cc[VTIME] = 0;

        if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
            return Result<void, std::string>::err("Failed to set raw mode");
        }

        m_raw_mode_enabled = true;
        return Result<void, std::string>::ok();
    }

    void disable_raw_mode() override {
        if (!m_raw_mode_enabled) return;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &m_original);
        m_raw_mode_enabled = false;
    }

    char32_t read_char() override {
        unsigned char buf[4];
        ssize_t n;
        do {
            n = read(STDIN_FILENO, buf, 1);
        } while (n < 0 && errno == EINTR);
        if (n <= 0) return WEOF;

        if (buf[0] == 0x1b) {
            unsigned char seq[2];
            if (read(STDIN_FILENO, seq, 1) != 1) return 0x1b;
            if (seq[0] == '[') {
                if (read(STDIN_FILENO, seq, 1) != 1) return 0x1b;
                switch (seq[0]) {
                    case 'A': return KEY_ARROW_UP;
                    case 'B': return KEY_ARROW_DOWN;
                    case 'C': return KEY_ARROW_RIGHT;
                    case 'D': return KEY_ARROW_LEFT;
                    case 'H': return KEY_HOME;
                    case 'F': return KEY_END;
                    case '3': {
                        if (read(STDIN_FILENO, seq, 1) == 1 && seq[0] == '~')
                            return KEY_DELETE;
                        return 0x1b;
                    }
                    case '1': {
                        unsigned char tilde;
                        if (read(STDIN_FILENO, &tilde, 1) != 1) return 0x1b;
                        if (tilde == '~') return KEY_HOME;
                        if (tilde == ';') {
                            unsigned char mod;
                            if (read(STDIN_FILENO, &mod, 1) != 1) return 0x1b;
                            unsigned char final;
                            if (read(STDIN_FILENO, &final, 1) != 1) return 0x1b;
                            if (mod == '5' && final == 'C') return KEY_CTRL_ARROW_RIGHT;
                            if (mod == '5' && final == 'D') return KEY_CTRL_ARROW_LEFT;
                        }
                        return 0x1b;
                    }
                    case '5': {
                        unsigned char tilde;
                        if (read(STDIN_FILENO, &tilde, 1) == 1 && tilde == '~')
                            return KEY_HOME;
                        return 0x1b;
                    }
                    case '4': {
                        unsigned char tilde;
                        if (read(STDIN_FILENO, &tilde, 1) == 1 && tilde == '~')
                            return KEY_END;
                        return 0x1b;
                    }
                    default: return 0x1b;
                }
            }
            if (seq[0] == 'O') {
                if (read(STDIN_FILENO, seq, 1) != 1) return 0x1b;
                switch (seq[0]) {
                    case 'H': return KEY_HOME;
                    case 'F': return KEY_END;
                    default: return 0x1b;
                }
            }
            return 0x1b;
        }

        if ((buf[0] & 0x80) == 0) {
            if (buf[0] == 0x03) return KEY_CTRL_C;
            if (buf[0] == 0x0f) return KEY_CTRL_O;
            return buf[0];
        }

        size_t extra;
        if ((buf[0] & 0xE0) == 0xC0) extra = 1;
        else if ((buf[0] & 0xF0) == 0xE0) extra = 2;
        else if ((buf[0] & 0xF8) == 0xF0) extra = 3;
        else return 0xFFFD;

        if (read(STDIN_FILENO, buf + 1, extra) != static_cast<ssize_t>(extra))
            return 0xFFFD;

        char32_t cp;
        if (extra == 1) cp = buf[0] & 0x1F;
        else if (extra == 2) cp = buf[0] & 0x0F;
        else cp = buf[0] & 0x07;

        for (size_t i = 1; i <= extra; ++i)
            cp = (cp << 6) | (buf[i] & 0x3F);

        return cp;
    }

    void write_output(std::string_view text) override {
        if (text.empty()) return;
        fwrite(text.data(), 1, text.size(), stdout);
    }

    void move_cursor(int cols) override {
        if (cols == 0) return;
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
        struct winsize ws;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
            return ws.ws_col;
        }
        char* cols = getenv("COLUMNS");
        if (cols) {
            int val = atoi(cols);
            if (val > 0) return val;
        }
        return 80;
    }

    int get_terminal_height() override {
        struct winsize ws;
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) {
            return ws.ws_row;
        }
        char* rows = getenv("LINES");
        if (rows) {
            int val = atoi(rows);
            if (val > 0) return val;
        }
        return 24;
    }

    void flush() override {
        fflush(stdout);
    }

    int put_codepoint(const char* utf8_data, size_t length, int expected_width) override {
        fwrite(utf8_data, 1, length, stdout);
        fflush(stdout);
        return expected_width;
    }

private:
    struct termios m_original;
    bool m_raw_mode_enabled = false;
};

std::unique_ptr<IPlatform> create_platform() {
    return std::make_unique<PosixPlatform>();
}

} // namespace agent
