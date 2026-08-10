/**
 * @file platform_posix.cpp
 * @brief POSIX (Linux/macOS) termios 平台实现
 * @version 1.0.0
 */

#include "tui/core/platform/i_platform.h"
#include "tui/core/platform/vt_input_decoder.h"
#include "liblogger/logger.h"

#include <termios.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <csignal>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>

namespace tui {

using namespace agent;  // P0: tui→agent 类型引用过渡方案，后续 P2/P3 收紧到显式前缀

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
static constexpr char32_t KEY_RESIZE           = 0xE00B;  // 终端尺寸变更（SIGWINCH）

/// @brief ESC 序列续字节试探超时（毫秒）
/// @details 收到独立 ESC 前缀后在此窗口内等待续字节；超时判定为孤立 ESC（取消键）。
///          需大于序列组包间隔又足够短，Linux 终端方向键序列连续到达通常 < 16ms。
static constexpr int ESC_SEQ_TIMEOUT_MS = 50;

// SIGWINCH 到 KEY_RESIZE 的桥接：信号处理器只置 flag，read_char() 检测后返回 KEY_RESIZE。
// 使用 sig_atomic_t 保证信号上下文写入的原子性。
static volatile sig_atomic_t g_resize_pending = 0;

static void sigwinch_handler(int /*signum*/) {
    g_resize_pending = 1;
}

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

        // 启用 bracketed paste：终端把粘贴内容包裹在 ESC[200~ / ESC[201~ 中
        write_output("\x1b[?2004h");

        // 创建 self-pipe，用于跨线程唤醒阻塞的 read_char（notify_wake 写一字节触发）
        if (pipe(m_wake_pipe) != 0) {
            m_wake_pipe[0] = m_wake_pipe[1] = -1;
            // 自检失败：notify_wake 将无法唤醒主循环，AskUser 等跨线程通知不可用
            LOG_ERROR("[PosixPlatform] pipe() failed (errno={}), cross-thread wake disabled", errno);
        } else {
            // 读端设为非阻塞：drain（while read > 0）读空后返回 EAGAIN 而非永久阻塞，
            // 否则第二次 read 会在空管道上挂死，导致 KEY_WAKE 永远无法返回主循环。
            int flags = fcntl(m_wake_pipe[0], F_GETFL, 0);
            if (flags < 0 || fcntl(m_wake_pipe[0], F_SETFL, flags | O_NONBLOCK) < 0) {
                LOG_ERROR("[PosixPlatform] fcntl(O_NONBLOCK) failed (errno={}), cross-thread wake disabled", errno);
            }
        }

        // 注册 SIGWINCH 处理器：终端尺寸变更时置 flag，read_char() 据此返回 KEY_RESIZE。
        // 保存旧 handler 以便 disable_raw_mode 恢复。
        m_sigwinch_installed = (signal(SIGWINCH, sigwinch_handler) != SIG_ERR);

        m_raw_mode_enabled = true;
        return Result<void, std::string>::ok();
    }

    void disable_raw_mode() override {
        if (!m_raw_mode_enabled) return;
        // 关闭 bracketed paste（在恢复 termios 之前发送，此时 OPOST 仍被禁用）
        write_output("\x1b[?2004l");
        if (m_sigwinch_installed) {
            signal(SIGWINCH, SIG_DFL);
            m_sigwinch_installed = false;
        }
        // 关闭 self-pipe
        if (m_wake_pipe[0] != -1) close(m_wake_pipe[0]);
        if (m_wake_pipe[1] != -1) close(m_wake_pipe[1]);
        m_wake_pipe[0] = m_wake_pipe[1] = -1;
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &m_original);
        m_raw_mode_enabled = false;
    }

    void notify_wake() override {
        LOG_INFO("[PosixPlatform] notify_wake called, pipe_w={}", m_wake_pipe[1]);
        if (m_wake_pipe[1] != -1) {
            char byte = 1;
            ssize_t n = write(m_wake_pipe[1], &byte, 1);
            LOG_INFO("[PosixPlatform] notify_wake wrote {} bytes", n);
            (void)n;  // 非阻塞写失败可忽略（管道缓冲满说明已有待处理唤醒）
        }
    }

    char32_t read_char() override {
        unsigned char buf[4];
        size_t buf_len = 0;
        size_t buf_cap = 1;  // 当前码点总字节数（首字节确定；1 = 尚未确定）

        while (true) {
            // 优先检查 resize flag：捕获两次 read() 之间到达的 SIGWINCH（避免丢失字节）
            if (g_resize_pending) {
                g_resize_pending = 0;
                return KEY_RESIZE;
            }

            // 每读一字节前都 poll stdin + self-pipe：任一就绪即继续。
            // 消除"read 半途阻塞"竞态——AskUser 事件在任意读取间隙到达
            // （含 UTF-8 续字节、ESC 序列中间态）都能立即唤醒主循环。
            if (m_wake_pipe[0] != -1) {
                struct pollfd fds[2];
                fds[0].fd = STDIN_FILENO;
                fds[0].events = POLLIN;
                fds[0].revents = 0;
                fds[1].fd = m_wake_pipe[0];
                fds[1].events = POLLIN;
                fds[1].revents = 0;
                // 解码器处于序列中间态（已收到 ESC 前缀）时用短超时试探续字节：
                // 有续字节则继续组码；超时则判定为孤立 ESC 返回 0x1B。
                // 否则孤立 ESC 后会永久阻塞在 poll(-1)，ChoicePanel 无法用 Esc 取消。
                int timeout_ms = m_vt_decoder.pending() ? ESC_SEQ_TIMEOUT_MS : -1;
                int pr = poll(fds, 2, timeout_ms);
                LOG_INFO("[PosixPlatform] read_char poll returned, pr={}, stdin_rev={}, pipe_rev={}",
                         pr, fds[0].revents, fds[1].revents);
                if (pr < 0) {
                    if (errno == EINTR) continue;  // 信号打断：回顶部重查 resize/pipe
                    return WEOF;                   // poll 失败（非 EINTR）按无输入处理
                }
                if (pr == 0) {
                    // 序列中间态超时：孤立 ESC（用户按 Esc 取消），丢弃半截序列
                    m_vt_decoder.reset();
                    return 0x1B;
                }
                // self-pipe 就绪：丢弃半截输入序列，清空管道并返回 KEY_WAKE
                if (fds[1].revents & POLLIN) {
                    LOG_INFO("[PosixPlatform] read_char returning KEY_WAKE");
                    m_vt_decoder.reset();
                    char dummy;
                    while (read(m_wake_pipe[0], &dummy, 1) > 0) {}
                    return KEY_WAKE;
                }
                // self-pipe 未就绪而 poll 返回：stdin 有字节，继续读取
            }

            // stdin 就绪：读取一字节（poll 已保证可读；EINTR 回顶部重试）
            ssize_t n = read(STDIN_FILENO, buf + buf_len, 1);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) return WEOF;

            // 解码器序列中间态（已收到 ESC）：续字节直接喂解码器，不并入 UTF-8 缓冲。
            // 修复：方向键等 `ESC [ A` 序列中，`[` 和 `A` 原会因 buf_len 递增
            // 误入 UTF-8 续字节分支（把 0x1B 当头字节算出垃圾码），并使解码器
            // 滞留在 Esc 态把 `A` 当孤立 ESC 返回 0x1B（ChoicePanel 误判为取消）。
            if (m_vt_decoder.pending()) {
                switch (m_vt_decoder.feed(buf[buf_len])) {
                    case VtInputDecoder::Event::Char:
                        return m_vt_decoder.code();
                    default:
                        buf_len = 0;  // 序列字节已消费，重置 UTF-8 缓冲
                        continue;
                }
            }

            ++buf_len;

            if (buf_len == 1) {
                if ((buf[0] & 0x80) == 0) {
                    // 单字节 ASCII：Ctrl 组合先映射（不喂解码器）
                    if (buf[0] == 0x03) return KEY_CTRL_C;
                    if (buf[0] == 0x0f) return KEY_CTRL_O;
                    // 喂解码器（方向键/功能键序列、bracketed paste 标记）；
                    // 序列未完成（None）时回顶部 poll 等下一字节（可被 pipe 中断）
                    switch (m_vt_decoder.feed(buf[0])) {
                        case VtInputDecoder::Event::Char:
                            return m_vt_decoder.code();
                        default:
                            buf_len = 0;  // 序列中间态：重置缓冲，续字节走 pending 分支
                            continue;
                    }
                }
                // 多字节 UTF-8：首字节确定总长度，继续收后续字节
                if ((buf[0] & 0xE0) == 0xC0) buf_cap = 2;
                else if ((buf[0] & 0xF0) == 0xE0) buf_cap = 3;
                else if ((buf[0] & 0xF8) == 0xF0) buf_cap = 4;
                else return 0xFFFD;
                continue;
            }

            // UTF-8 续字节
            if (buf_len < buf_cap) continue;

            char32_t cp;
            if (buf_cap == 2) cp = buf[0] & 0x1F;
            else if (buf_cap == 3) cp = buf[0] & 0x0F;
            else cp = buf[0] & 0x07;
            for (size_t i = 1; i < buf_cap; ++i)
                cp = (cp << 6) | (buf[i] & 0x3F);

            // 多字节码点：粘贴内容里的中文等，直接返回（换行转换由解码器处理 ASCII 部分）
            return cp;
        }
    }

    void write_output(std::string_view text) override {
        if (text.empty()) return;
        if (!m_raw_mode_enabled) {
            // 非 raw mode：OPOST 启用，终端驱动自动转换 \n → \r\n
            fwrite(text.data(), 1, text.size(), stdout);
            return;
        }
        // raw mode 下 OPOST 被禁用，\n 只下移一行不回列首。
        // 手动翻译裸 \n → \r\n（已是 \r\n 的不重复翻译）。
        size_t start = 0;
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) {
                if (i > start) {
                    fwrite(text.data() + start, 1, i - start, stdout);
                }
                fwrite("\r\n", 1, 2, stdout);
                start = i + 1;
            }
        }
        if (start < text.size()) {
            fwrite(text.data() + start, 1, text.size() - start, stdout);
        }
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
    bool m_sigwinch_installed = false;  ///< SIGWINCH 处理器是否已注册
    VtInputDecoder m_vt_decoder;        ///< VT 输入序列解码器
    int m_wake_pipe[2] = {-1, -1};      ///< self-pipe：notify_wake 写端，read_char poll 读端
};

std::unique_ptr<IPlatform> create_platform() {
    return std::make_unique<PosixPlatform>();
}

} // namespace tui
