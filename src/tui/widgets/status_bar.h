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

namespace tui {

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
    void set_context_limit(int32_t limit);  // 模型上下文窗口（token），用于进度条分母
    void set_cache_read_tokens(int32_t count);  // Anthropic prompt cache 命中 token 数（0 表示无命中）
    void set_ds_cache_hit_rate(int32_t rate);   // DeepSeek 会话级缓存命中率（0-100，-1 表示无数据）
    void set_thinking_seconds(int32_t) {}
    void set_tool_name(const std::string&) {}
    void start_session_timer();

    void subscribe_events() {}
    void unsubscribe_events() {}

    void render();
    void clear();

    /// @brief 清除上次渲染行号缓存（resize 后强制重新定位 + 擦除旧行）
    void invalidate_last_row() { m_last_rendered_row = 0; }

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
    int32_t m_context_limit = 0;  // 0 表示未知，进度条按 8192 兜底
    int32_t m_cache_read_tokens = 0;  // Anthropic prompt cache 命中 token 数（0 表示无命中）
    int32_t m_ds_cache_hit_rate = -1;  // DeepSeek 会话级缓存命中率（-1 表示无数据）
    std::chrono::steady_clock::time_point m_session_start;
    mutable std::string m_last_bar;
    mutable int m_last_rendered_row = 0;  ///< 上次渲染的状态栏行号（用于 resize 后擦除旧行）
    std::atomic<int> m_frame{0};

    static constexpr int SPINNER_FRAME_COUNT = 10;
};

} // namespace tui
