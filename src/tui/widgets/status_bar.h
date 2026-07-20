/**
 * @file status_bar.h
 * @brief 输入栏上方的状态栏
 * @details 在输入栏上方渲染会话状态信息
 *
 * 格式: [model] │ project |Context ░░░░ XX% (time)
 * @version 4.0.0
 */

#pragma once

#include <string>
#include <chrono>
#include <mutex>
#include <memory>
#include <atomic>

#include "tui/core/tui_state.h"

namespace agent {

class Terminal;

/**
 * @brief 状态栏
 * @details 在输入栏上方渲染会话状态信息
 *
 * 格式: ⠋ [model] │ project |Context ░░ XX% (time)
 *       LLM 请求时显示 Braille 旋转动画
 */
class StatusBar {
public:
    explicit StatusBar(Terminal* terminal);

    void set_state(TuiState state);
    void set_model_name(const std::string& name);
    void set_project_name(const std::string& name);
    void set_token_count(int32_t count);
    void set_thinking_seconds(int32_t) {}
    void set_tool_name(const std::string&) {}
    void start_session_timer();

    void subscribe_events() {}
    void unsubscribe_events() {}

    void render();
    void clear();

    void advance_frame();

    bool is_active_state() const;

private:
    std::string format_duration(int64_t seconds) const;
    std::string format_bar() const;
    std::string format_dynamic_bar() const;
    std::string get_spinner_char() const;
    std::string get_animated_color() const;

    Terminal* m_terminal;
    mutable std::mutex m_mutex;

    TuiState m_state = TuiState::IDLE;
    std::string m_model_name = "unknown";
    std::string m_project_name = "workx";
    int32_t m_token_count = 0;
    std::chrono::steady_clock::time_point m_session_start;
    mutable std::string m_last_bar;
    std::atomic<int> m_frame{0};

    static constexpr int SPINNER_FRAME_COUNT = 10;
};

} // namespace agent
