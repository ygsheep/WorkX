/**
 * @file spinner.cpp
 * @brief 思考计时动画实现
 * @version 2.0.0
 */

#include "tui/render/spinner.h"
#include "tui/core/terminal.h"

#include <chrono>

namespace workx {

Spinner::Spinner(Terminal* terminal)
    : m_terminal(terminal)
{
}

Spinner::~Spinner() {
    stop();
}

void Spinner::start(std::string_view msg) {
    if (m_running) return;

    m_message = msg;
    m_start_time = std::chrono::steady_clock::now();
    m_running = true;

    m_thread = std::thread(&Spinner::run, this);
}

void Spinner::stop() {
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (!m_running) return;
        m_running = false;
        m_cv.notify_all();
    }

    if (m_thread.joinable()) {
        if (m_thread.get_id() == std::this_thread::get_id()) {
            m_thread.detach();
        } else {
            m_thread.join();
        }
    }
}

void Spinner::set_update_callback(UpdateCallback cb) {
    m_update_callback = std::move(cb);
}

int32_t Spinner::elapsed_seconds() const {
    if (!m_running) return 0;
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - m_start_time).count();
    return static_cast<int32_t>(elapsed);
}

void Spinner::run() {
    while (true) {
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            if (m_cv.wait_for(lock, std::chrono::milliseconds(FRAME_INTERVAL_MS),
                [this]() { return !m_running; })) {
                break;
            }
        }

        // 通知回调更新思考计时
        if (m_update_callback) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - m_start_time).count();
            m_update_callback(static_cast<int32_t>(elapsed));
        }
    }
}

} // namespace workx
