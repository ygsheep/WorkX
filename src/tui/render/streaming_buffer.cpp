/**
 * @file streaming_buffer.cpp
 * @brief Token 流式缓冲区实现
 * @version 2.0.0
 */

#include "tui/render/streaming_buffer.h"
#include "tui/core/terminal.h"

#include <chrono>

namespace agent {

StreamingBuffer::StreamingBuffer(Terminal* terminal)
    : m_terminal(terminal)
{
}

StreamingBuffer::~StreamingBuffer() {
    stop();
}

void StreamingBuffer::push(std::string_view text) {
    if (text.empty()) return;

    bool was_empty;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        was_empty = m_buffer.empty();
        m_buffer.append(text);
    }

    // 如果缓冲区之前为空，通知刷新线程
    if (was_empty && m_running) {
        m_cv.notify_one();
    }
}

void StreamingBuffer::flush_now() {
    std::string chunk;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_buffer.empty()) return;
        chunk = std::move(m_buffer);
        m_buffer.clear();
    }

    m_terminal->write_safe(chunk, true);
}

void StreamingBuffer::start() {
    if (m_running) return;
    m_running = true;
    m_thread = std::thread(&StreamingBuffer::flush_loop, this);
}

void StreamingBuffer::stop() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_running) return;
        m_running = false;
        m_cv.notify_all();
    }

    if (m_thread.joinable()) {
        m_thread.join();
    }

    // 刷新剩余内容
    flush_now();
}

void StreamingBuffer::flush_loop() {
    while (true) {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (m_cv.wait_for(lock, std::chrono::milliseconds(FLUSH_INTERVAL_MS),
                [this]() { return !m_running; })) {
                // 被通知停止
                break;
            }
        }

        // 刷新缓冲区
        flush_now();
    }
}

} // namespace workx
