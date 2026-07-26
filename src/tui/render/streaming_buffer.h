/**
 * @file streaming_buffer.h
 * @brief Token 流式缓冲区
 * @details 减少 mutex 锁竞争：Token 写入先进入缓冲区，刷新线程每 16ms 批量写入
 * @version 2.0.0
 */

#pragma once

#include <string>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <functional>

namespace tui {

class Terminal;

/**
 * @brief 流式缓冲区
 * @details 将高频的 token 写入聚合为低频的批量写入（~60fps）
 *
 * 优化点：
 * - Token 写入先进入 m_buffer（快速路径，仅一次 mutex）
 * - 刷新线程每 16ms 将 m_buffer 内容一次性写入终端
 * - 停止流式时立即 flush 剩余内容
 */
class StreamingBuffer {
public:
    explicit StreamingBuffer(Terminal* terminal);
    ~StreamingBuffer();

    /// @brief 写入文本到缓冲区
    void push(std::string_view text);

    /// @brief 立即刷新所有缓冲内容到终端
    void flush_now();

    /// @brief 启动缓冲区刷新线程
    void start();

    /// @brief 停止缓冲区并刷新剩余内容
    void stop();

private:
    void flush_loop();

    Terminal* m_terminal;
    std::string m_buffer;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_thread;
    std::atomic<bool> m_running{false};

    static constexpr int FLUSH_INTERVAL_MS = 16;  // ~60fps
};

} // namespace tui
