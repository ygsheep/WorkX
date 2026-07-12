/**
 * @file status_bar.cpp
 * @brief 输入栏上方的状态栏实现
 * @version 4.0.0
 */

#include "tui/widgets/status_bar.h"
#include "tui/core/terminal.h"

#include <cstdio>
#include <cmath>

namespace agent {

static const char* SPINNER_FRAMES[] = {
    "\xe2\xa0\x8b",
    "\xe2\xa0\x99",
    "\xe2\xa0\xb9",
    "\xe2\xa0\xb8",
    "\xe2\xa0\xbc",
    "\xe2\xa0\xb4",
    "\xe2\xa0\xa6",
    "\xe2\xa0\xa7",
    "\xe2\xa0\x87",
    "\xe2\xa0\x8f",
};

StatusBar::StatusBar(Terminal* terminal)
    : m_terminal(terminal)
{
}

void StatusBar::set_state(TuiState state) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_state = state;
    if (state == TuiState::IDLE) {
        m_frame.store(0, std::memory_order_relaxed);
    }
}

void StatusBar::set_model_name(const std::string& name) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_model_name = name;
}

void StatusBar::set_project_name(const std::string& name) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_project_name = name;
}

void StatusBar::set_token_count(int32_t count) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_token_count = count;
}

void StatusBar::start_session_timer() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_session_start = std::chrono::steady_clock::now();
}

void StatusBar::advance_frame() {
    m_frame.fetch_add(1, std::memory_order_relaxed);
}

bool StatusBar::is_active_state() const {
    return m_state != TuiState::IDLE;
}

std::string StatusBar::get_spinner_char() const {
    int idx = m_frame.load(std::memory_order_relaxed) % SPINNER_FRAME_COUNT;
    return SPINNER_FRAMES[idx];
}

std::string StatusBar::get_animated_color() const {
    int frame = m_frame.load(std::memory_order_relaxed);
    int hue = 20 + ((frame * 5) % 26);
    double h = hue / 60.0;
    int sector = static_cast<int>(h) % 6;
    double f = h - static_cast<int>(h);
    int r = 0, g = 0, b = 0;
    switch (sector) {
        case 0: r = 255; g = static_cast<int>(255 * f); b = 0; break;
        case 1: r = static_cast<int>(255 * (1 - f)); g = 255; b = 0; break;
        default: r = 255; g = 165; b = 0; break;
    }
    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[38;2;%d;%d;%dm", r, g, b);
    return buf;
}

void StatusBar::render() {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto bar = format_bar();
    if (bar.empty()) return;

    if (bar == m_last_bar) return;
    m_last_bar = bar;

    int height = m_terminal->get_terminal_height();

    int status_row = height - 2;
    if (status_row < 1) return;

    char pos_cmd[32];
    snprintf(pos_cmd, sizeof(pos_cmd), "\x1b[%d;1H", status_row);

    std::string update = std::string("\x1b[?25l")
        + pos_cmd
        + "\x1b[2K"
        + bar
        + "\x1b[0m"
        + "\x1b[?25h";

    m_terminal->write_safe(update);
}

void StatusBar::clear() {
    int height = m_terminal->get_terminal_height();
    int status_row = height - 2;
    if (status_row < 1) return;

    m_last_bar.clear();

    char pos_cmd[32];
    snprintf(pos_cmd, sizeof(pos_cmd), "\x1b[%d;1H", status_row);

    std::string update = std::string("\x1b[?25l")
        + pos_cmd + "\x1b[2K"
        + "\x1b[?25h";

    m_terminal->write_safe(update);
}

std::string StatusBar::format_duration(int64_t seconds) const {
    if (seconds < 60) {
        return std::to_string(seconds) + "s";
    }
    int64_t mins = seconds / 60;
    int64_t secs = seconds % 60;
    if (mins < 60) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%lldm %llds", (long long)mins, (long long)secs);
        return buf;
    }
    int64_t hours = mins / 60;
    mins = mins % 60;
    char buf[32];
    snprintf(buf, sizeof(buf), "%lldh%02lldm", (long long)hours, (long long)mins);
    return buf;
}

std::string StatusBar::format_dynamic_bar() const {
    std::string anim_color = get_animated_color();
    std::string gray  = "\x1b[90m";
    std::string reset = "\x1b[0m";

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - m_session_start).count();
    std::string time_str = format_duration(elapsed);

    std::string token_str;
    if (m_token_count >= 1000) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1fk", m_token_count / 1000.0);
        token_str = buf;
    } else if (m_token_count > 0) {
        token_str = std::to_string(m_token_count);
    }

    std::string bar = " "
        + anim_color + get_spinner_char() + " Thinking\xe2\x80\xa6" + reset
        + gray + " (" + time_str;
    if (!token_str.empty()) {
        bar += " \xc2\xb7 \xe2\x86\x93 " + token_str + " tokens";
    }
    bar += ")" + reset;
    return bar;
}

std::string StatusBar::format_bar() const {
    if (m_state != TuiState::IDLE) {
        return format_dynamic_bar();
    }

    std::string green = "\x1b[32m";
    std::string gray  = "\x1b[90m";
    std::string reset = "\x1b[0m";

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        now - m_session_start).count();
    std::string time_str = format_duration(elapsed);

    int ctx_pct = std::min(m_token_count * 100 / 4096, 100);
    int filled = ctx_pct / 10;

    std::string bar_str;
    for (int i = 0; i < 10; ++i) {
        bar_str += (i < filled) ? "\xe2\x96\x88" : "\xe2\x96\x91";
    }

    std::string bar = " "
        + green + "[" + m_model_name + "]" + reset
        + " \xe2\x94\x82 " + m_project_name
        + " |Context " + bar_str
        + " " + std::to_string(ctx_pct) + "%"
        + gray + " (" + time_str + ")" + reset;

    return bar;
}

} // namespace workx
