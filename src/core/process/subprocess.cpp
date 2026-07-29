/**
 * @file subprocess.cpp
 * @brief subprocess::exec() 跨平台实现
 * @details
 * Windows 实现：
 *   - CreatePipe 创建 stdout/stderr 管道（继承到子进程，父进程读端）
 *   - CreateProcessW 启动子进程
 *   - ReadFile 循环读取管道 + 检查超时/取消
 *   - TerminateProcess 终止（无 SIGTERM 概念）
 *   - 编码：子进程输出按 UTF-8 解码，回退 MultiByteToWideChar(CP_ACP)
 *
 * POSIX 实现：
 *   - pipe() 创建 stdout/stderr 管道
 *   - fork() + execvp() 启动子进程
 *   - poll() 等待管道可读 + 检查超时/取消
 *   - kill(SIGTERM) → 5s 后 kill(SIGKILL) 升级（防卡在不可中断 I/O）
 *   - 编码：透传（通常 UTF-8）
 *
 * @version 1.0.0
 * @date 2026-07
 */

#include "core/process/subprocess.h"

#include <algorithm>
#include <cstring>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace agent::process {

namespace {

#ifdef _WIN32
// ============================================================
// Windows 实现
// ============================================================

/// Auto-handle：RAII 包装 Windows HANDLE，析构时 CloseHandle
struct HandleGuard {
    HANDLE h = INVALID_HANDLE_VALUE;
    explicit HandleGuard(HANDLE handle = INVALID_HANDLE_VALUE) : h(handle) {}
    ~HandleGuard() { if (h != INVALID_HANDLE_VALUE && h != nullptr) CloseHandle(h); }
    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;
    operator HANDLE() const { return h; }
    HANDLE* operator&() { return &h; }
    /// 释放所有权（返回 handle，不再负责关闭）
    HANDLE release() { HANDLE tmp = h; h = INVALID_HANDLE_VALUE; return tmp; }
};

/// 创建匿名管道，读端不继承（父进程持有），写端可继承（子进程持有）
bool create_inheritable_pipe(HANDLE* read_handle, HANDLE* write_handle) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(read_handle, write_handle, &sa, 0)) {
        return false;
    }
    // 读端设为不可继承，避免子进程持有导致管道无法关闭
    if (!SetHandleInformation(*read_handle, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(*read_handle);
        CloseHandle(*write_handle);
        return false;
    }
    return true;
}

/// 将 UTF-8 字符串转换为 UTF-16 宽字符串
std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring w(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
    return w;
}

/// 将宽字符串按 UTF-8 编码转回窄字符串
std::string wide_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), len, nullptr, nullptr);
    return s;
}

/// 从管道读取数据（非阻塞模式）
/// 返回值：true 表示读到了数据，false 表示暂时无数据或 EOF（调用方需另行检测 EOF）
/// @details 先用 PeekNamedPipe 探测数据量；若 Peek 失败（管道写端已关闭 = broken pipe），
///          仍尝试 ReadFile，因为管道缓冲区可能还有未读数据（ReadFile 在 broken pipe
///          上会立即返回剩余数据或失败，不会阻塞）。
bool read_pipe(HANDLE pipe, std::string& buf, size_t max_bytes) {
    if (buf.size() >= max_bytes) return false;

    DWORD available = 0;
    bool peek_ok = PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr);

    // Peek 成功但无数据：管道仍开着，只是暂时没数据
    if (peek_ok && available == 0) return false;

    // Peek 成功且有数据：按 available 量读
    // Peek 失败（broken pipe）：试读 8192 字节，可能拿到缓冲区剩余数据
    DWORD to_read = static_cast<DWORD>(std::min<size_t>(
        (peek_ok && available > 0) ? available : 8192,
        max_bytes - buf.size()
    ));
    if (to_read == 0) return false;

    std::string chunk(to_read, '\0');
    DWORD read_bytes = 0;
    if (!ReadFile(pipe, chunk.data(), to_read, &read_bytes, nullptr) || read_bytes == 0) {
        return false; // 真正 EOF（无更多数据）
    }
    chunk.resize(read_bytes);
    buf += chunk;
    return true;
}

/// 判断参数是否需要加引号（含空格/制表符/引号时需要）
bool needs_quotes(const std::string& s) {
    if (s.empty()) return true;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == '"') return true;
    }
    return false;
}

/// 转义单个参数：需要时加引号并转义内部引号
/// @details 遵循 CommandLineToArgvW 规则：每个 " 变成 \"，整体用 "..." 包裹
std::string escape_arg(const std::string& arg) {
    if (!needs_quotes(arg)) return arg;
    std::string result;
    result.reserve(arg.size() + 4);
    result += '"';
    for (char c : arg) {
        if (c == '"') result += "\\\"";
        else result += c;
    }
    result += '"';
    return result;
}

ResultV2<ExecOutput> exec_windows(const std::string& cmd, const ExecOptions& opts) {
    // 1. 构建命令行：cmd + arg1 + arg2 + ...
    //    只对含空格/引号的参数加引号，避免 cmd.exe "/c" 被引号包裹后无法识别
    //    （cmd.exe 不识别 "/c" 为 /c 开关，会导致命令解析失败）
    std::string cmdline = escape_arg(cmd);
    for (const auto& arg : opts.args) {
        cmdline += ' ';
        cmdline += escape_arg(arg);
    }
    std::wstring wcmdline = utf8_to_wide(cmdline);
    std::wstring wcwd = utf8_to_wide(opts.cwd);

    // 2. 创建 stdout/stderr 管道
    HANDLE stdout_read = INVALID_HANDLE_VALUE, stdout_write = INVALID_HANDLE_VALUE;
    HANDLE stderr_read = INVALID_HANDLE_VALUE, stderr_write = INVALID_HANDLE_VALUE;
    if (!create_inheritable_pipe(&stdout_read, &stdout_write)) {
        return ResultV2<ExecOutput>::err(Error::Code::InternalError,
            "CreatePipe(stdout) failed", "subprocess::exec");
    }
    HandleGuard g_stdout_read(stdout_read), g_stdout_write(stdout_write);
    if (!create_inheritable_pipe(&stderr_read, &stderr_write)) {
        return ResultV2<ExecOutput>::err(Error::Code::InternalError,
            "CreatePipe(stderr) failed", "subprocess::exec");
    }
    HandleGuard g_stderr_read(stderr_read), g_stderr_write(stderr_write);

    // 3. 启动子进程
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = stdout_write;
    si.hStdError = stderr_write;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};

    if (!CreateProcessW(
            nullptr,                       // lpApplicationName（nullptr 表示从 cmdline 解析）
            wcmdline.data(),               // lpCommandLine（可写缓冲区）
            nullptr, nullptr,              // 进程/线程安全属性
            TRUE,                          // bInheritHandles
            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,  // dwCreationFlags
            nullptr,                       // lpEnvironment（继承父进程）
            wcwd.empty() ? nullptr : wcwd.c_str(),
            &si, &pi)) {
        DWORD err = GetLastError();
        return ResultV2<ExecOutput>::err(Error::Code::ResourceNotFound,
            "CreateProcessW failed for '" + cmd + "' (error " + std::to_string(err) + ")",
            "subprocess::exec");
    }
    HandleGuard g_process(pi.hProcess), g_thread(pi.hThread);

    // 4. 关闭父进程持有的写端，让子进程的 EOF 能传到读端
    CloseHandle(g_stdout_write.release());
    CloseHandle(g_stderr_write.release());

    // 5. 循环读取管道 + 检查超时/取消
    ExecOutput output;
    const auto start = std::chrono::steady_clock::now();
    bool stdout_open = true, stderr_open = true;

    while (stdout_open || stderr_open) {
        // 检查取消
        if (opts.is_cancelled && opts.is_cancelled()) {
            TerminateProcess(pi.hProcess, 1);
            output.cancelled = true;
            break;
        }
        // 检查超时
        if (opts.timeout) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed >= *opts.timeout) {
                TerminateProcess(pi.hProcess, 1);
                output.timed_out = true;
                break;
            }
        }

        bool got_data = false;
        // 读 stdout
        if (stdout_open) {
            if (output.stdout_text.size() >= opts.max_output_bytes) {
                // 缓冲区已满，标记截断，读弃数据以防止子进程管道阻塞
                output.stdout_truncated = true;
                std::string discard;
                if (read_pipe(stdout_read, discard, 65536)) {
                    got_data = true;
                } else {
                    DWORD available = 0;
                    if (!PeekNamedPipe(stdout_read, nullptr, 0, nullptr, &available, nullptr) && available == 0) {
                        stdout_open = false;
                    }
                }
            } else {
                if (read_pipe(stdout_read, output.stdout_text, opts.max_output_bytes)) {
                    got_data = true;
                    // 读取后再次检查是否到达上限
                    if (output.stdout_text.size() >= opts.max_output_bytes) {
                        output.stdout_truncated = true;
                    }
                } else {
                    DWORD available = 0;
                    if (!PeekNamedPipe(stdout_read, nullptr, 0, nullptr, &available, nullptr) && available == 0) {
                        stdout_open = false;
                    }
                }
            }
        }

        // 读 stderr
        if (stderr_open) {
            if (output.stderr_text.size() >= opts.max_output_bytes) {
                output.stderr_truncated = true;
                std::string discard;
                if (read_pipe(stderr_read, discard, 65536)) {
                    got_data = true;
                } else {
                    DWORD available = 0;
                    if (!PeekNamedPipe(stderr_read, nullptr, 0, nullptr, &available, nullptr) && available == 0) {
                        stderr_open = false;
                    }
                }
            } else {
                if (read_pipe(stderr_read, output.stderr_text, opts.max_output_bytes)) {
                    got_data = true;
                    if (output.stderr_text.size() >= opts.max_output_bytes) {
                        output.stderr_truncated = true;
                    }
                } else {
                    DWORD available = 0;
                    if (!PeekNamedPipe(stderr_read, nullptr, 0, nullptr, &available, nullptr) && available == 0) {
                        stderr_open = false;
                    }
                }
            }
        }

        // 无数据时短暂 sleep，避免 busy loop
        if (!got_data) {
            // 检查子进程是否已退出
            DWORD exit_code = 0;
            if (GetExitCodeProcess(pi.hProcess, &exit_code) && exit_code != STILL_ACTIVE) {
                // 进程已退出，drain 剩余管道数据
                while (read_pipe(stdout_read, output.stdout_text, opts.max_output_bytes)) {}
                while (read_pipe(stderr_read, output.stderr_text, opts.max_output_bytes)) {}
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    // 6. 等待子进程完全退出
    if (!output.timed_out && !output.cancelled) {
        WaitForSingleObject(pi.hProcess, 5000);
    }

    // 7. 获取退出码
    DWORD exit_code = 0;
    if (GetExitCodeProcess(pi.hProcess, &exit_code)) {
        output.exit_code = static_cast<int>(exit_code);
    }

    return ResultV2<ExecOutput>::ok(std::move(output));
}

#else  // !_WIN32
// ============================================================
// POSIX 实现
// ============================================================

/// 设置文件描述符为非阻塞
bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

/// 从非阻塞 fd 读取数据到 buf
/// @return 读取字节数（>0 成功）；0 表示 EOF 或错误（调用方应关闭管道）；
///         -1 表示暂无数据（EAGAIN/EWOULDBLOCK，应继续轮询）
ssize_t read_nonblocking(int fd, std::string& buf, size_t max_bytes) {
    if (buf.size() >= max_bytes) return 0;
    size_t to_read = std::min<size_t>(8192, max_bytes - buf.size());
    std::string chunk(to_read, '\0');
    ssize_t n = read(fd, chunk.data(), to_read);
    if (n > 0) {
        chunk.resize(static_cast<size_t>(n));
        buf += chunk;
        return n;
    }
    if (n == 0) return 0; // EOF
    // EAGAIN/EWOULDBLOCK：暂无数据，继续轮询
    if (errno == EAGAIN || errno == EWOULDBLOCK) return -1;
    // 其他错误（EIO/EBADF 等）：视为 EOF，调用方应关闭管道
    return 0;
}

ResultV2<ExecOutput> exec_posix(const std::string& cmd, const ExecOptions& opts) {
    // 1. 创建 stdout/stderr 管道
    int stdout_pipe[2] = {-1, -1};
    int stderr_pipe[2] = {-1, -1};
    if (pipe(stdout_pipe) < 0) {
        return ResultV2<ExecOutput>::err(Error::Code::InternalError,
            "pipe(stdout) failed: " + std::string(strerror(errno)), "subprocess::exec");
    }
    if (pipe(stderr_pipe) < 0) {
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        return ResultV2<ExecOutput>::err(Error::Code::InternalError,
            "pipe(stderr) failed: " + std::string(strerror(errno)), "subprocess::exec");
    }

    // 2. fork
    pid_t pid = fork();
    if (pid < 0) {
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(stderr_pipe[0]); close(stderr_pipe[1]);
        return ResultV2<ExecOutput>::err(Error::Code::InternalError,
            "fork() failed: " + std::string(strerror(errno)), "subprocess::exec");
    }

    if (pid == 0) {
        // ---- 子进程 ----
        // 重定向 stdout/stderr 到管道写端
        dup2(stdout_pipe[1], STDOUT_FILENO);
        dup2(stderr_pipe[1], STDERR_FILENO);
        close(stdout_pipe[0]); close(stdout_pipe[1]);
        close(stderr_pipe[0]); close(stderr_pipe[1]);

        // 切换工作目录
        if (!opts.cwd.empty()) {
            if (chdir(opts.cwd.c_str()) != 0) {
                _exit(127);
            }
        }

        // 重置信号处理（子进程不应继承父进程的信号 handler）
        signal(SIGINT, SIG_DFL);
        signal(SIGQUIT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);

        // 构建 argv
        std::vector<const char*> argv;
        argv.reserve(opts.args.size() + 2);
        argv.push_back(cmd.c_str());
        for (const auto& arg : opts.args) {
            argv.push_back(arg.c_str());
        }
        argv.push_back(nullptr);

        execvp(cmd.c_str(), const_cast<char* const*>(argv.data()));
        // execvp 失败（命令不存在等）
        _exit(127);
    }

    // ---- 父进程 ----
    close(stdout_pipe[1]); close(stderr_pipe[1]);

    // 设置读端非阻塞
    set_nonblocking(stdout_pipe[0]);
    set_nonblocking(stderr_pipe[0]);

    // 3. poll 循环读取管道 + 检查超时/取消
    ExecOutput output;
    const auto start = std::chrono::steady_clock::now();
    bool stdout_open = true, stderr_open = true;

    while (stdout_open || stderr_open) {
        // 检查取消
        if (opts.is_cancelled && opts.is_cancelled()) {
            kill(pid, SIGTERM);
            // 给 5s 升级到 SIGKILL
            int status = 0;
            for (int i = 0; i < 50; ++i) { // 5s = 50 * 100ms
                pid_t w = waitpid(pid, &status, WNOHANG);
                if (w == pid || w == -1) goto kill_done_cancelled;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
        kill_done_cancelled:
            output.cancelled = true;
            break;
        }

        // 检查超时
        if (opts.timeout) {
            auto elapsed = std::chrono::steady_clock::now() - start;
            if (elapsed >= *opts.timeout) {
                kill(pid, SIGTERM);
                // 5s 升级 SIGKILL
                int status = 0;
                for (int i = 0; i < 50; ++i) {
                    pid_t w = waitpid(pid, &status, WNOHANG);
                    if (w == pid || w == -1) goto kill_done_timeout;
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
            kill_done_timeout:
                output.timed_out = true;
                break;
            }
        }

        // poll 管道
        struct pollfd fds[2];
        int nfds = 0;
        if (stdout_open) {
            fds[nfds].fd = stdout_pipe[0];
            fds[nfds].events = POLLIN;
            ++nfds;
        }
        if (stderr_open) {
            fds[nfds].fd = stderr_pipe[0];
            fds[nfds].events = POLLIN;
            ++nfds;
        }
        if (nfds == 0) break;

        // 100ms 超时让循环能定期检查 is_cancelled/timeout
        int poll_ret = poll(fds, nfds, 100);
        if (poll_ret < 0) {
            if (errno == EINTR) continue;
            break;
        }

        // 处理可读事件
        int fd_idx = 0;
        if (stdout_open) {
            if (fds[fd_idx].revents & (POLLIN | POLLHUP | POLLERR)) {
                if (output.stdout_text.size() >= opts.max_output_bytes) {
                    // 缓冲区已满，标记截断，读弃数据以防止子进程管道阻塞
                    output.stdout_truncated = true;
                    char discard[8192];
                    ssize_t n = read(stdout_pipe[0], discard, sizeof(discard));
                    if (n == 0) stdout_open = false; // EOF
                } else {
                    ssize_t n = read_nonblocking(stdout_pipe[0], output.stdout_text, opts.max_output_bytes);
                    if (n == 0) stdout_open = false; // EOF
                    else if (n > 0 && output.stdout_text.size() >= opts.max_output_bytes) {
                        output.stdout_truncated = true;
                    }
                }
            }
            ++fd_idx;
        }
        if (stderr_open) {
            if (fds[fd_idx].revents & (POLLIN | POLLHUP | POLLERR)) {
                if (output.stderr_text.size() >= opts.max_output_bytes) {
                    output.stderr_truncated = true;
                    char discard[8192];
                    ssize_t n = read(stderr_pipe[0], discard, sizeof(discard));
                    if (n == 0) stderr_open = false;
                } else {
                    ssize_t n = read_nonblocking(stderr_pipe[0], output.stderr_text, opts.max_output_bytes);
                    if (n == 0) stderr_open = false;
                    else if (n > 0 && output.stderr_text.size() >= opts.max_output_bytes) {
                        output.stderr_truncated = true;
                    }
                }
            }
        }
    }

    // 4. 等待子进程退出（如果还没被 kill）
    if (!output.timed_out && !output.cancelled) {
        int status = 0;
        if (waitpid(pid, &status, 0) > 0) {
            if (WIFEXITED(status)) {
                output.exit_code = WEXITSTATUS(status);
            } else if (WIFSIGNALED(status)) {
                output.exit_code = -1;
            }
        }
    } else {
        output.exit_code = -1;
    }

    close(stdout_pipe[0]);
    close(stderr_pipe[0]);
    return ResultV2<ExecOutput>::ok(std::move(output));
}
#endif // _WIN32

} // anonymous namespace

ResultV2<ExecOutput> exec(const std::string& cmd, const ExecOptions& opts) {
#ifdef _WIN32
    return exec_windows(cmd, opts);
#else
    return exec_posix(cmd, opts);
#endif
}

} // namespace agent::process
