/**
 * @file chat_renderer.h
 * @brief 事件订阅 + 终端渲染
 * @details 订阅 StreamToken/StreamDone/StreamError 等事件，调用 Terminal API 渲染
 *          使用 TuiStateMachine 管理状态转换，集成 StatusBar/OutputFormatter/StreamingBuffer
 * @version 2.0.0
 */

#pragma once

#include <string>
#include <memory>
#include <chrono>
#include <atomic>

#include "tui/core/tui_state.h"

namespace agent {

class Terminal;
class StatusBar;
class Spinner;
class EventToken;
class OutputFormatter;
class StreamingBuffer;

/**
 * @brief 聊天渲染器
 * @details TUI 核心渲染组件，订阅 EventBus 事件并驱动终端输出
 */
class ChatRenderer {
public:
    explicit ChatRenderer(Terminal* terminal);
    ~ChatRenderer();

    /// @brief 启动事件订阅
    void start();

    /// @brief 停止事件订阅
    void stop();

    /// @brief 切换思考视图（ctrl+o）
    void toggle_thinking_view();

    /// @brief 获取当前 TUI 状态
    TuiState state() const { return m_state_machine.current(); }

    /// @brief 获取 StatusBar（供 Terminal 使用）
    StatusBar* status_bar() const { return m_status_bar.get(); }

private:
    void transition_to(TuiState new_state);

    Terminal* m_terminal;
    std::unique_ptr<StatusBar> m_status_bar;
    std::unique_ptr<OutputFormatter> m_formatter;
    std::unique_ptr<StreamingBuffer> m_stream_buf;

    // 事件订阅 token
    std::unique_ptr<EventToken> m_token_status;
    std::unique_ptr<EventToken> m_token_stream;
    std::unique_ptr<EventToken> m_token_done;
    std::unique_ptr<EventToken> m_token_error;
    std::unique_ptr<EventToken> m_token_step;
    std::unique_ptr<EventToken> m_token_tool_call;
    std::unique_ptr<EventToken> m_token_tool_result;
    std::unique_ptr<EventToken> m_token_agent_done;

    // 状态机
    TuiStateMachine m_state_machine;

    // 思考内容管理
    bool m_spinner_active = false;
    std::string m_reasoning_buffer;          ///< 缓存的思考内容
    std::chrono::steady_clock::time_point m_thinking_start_time;
    bool m_viewing_thinking = false;         ///< 是否在思考视图
    int32_t m_thinking_seconds = 0;          ///< 思考持续秒数

    // 会话统计
    int32_t m_message_count = 0;
    int32_t m_total_tokens = 0;
    std::atomic<bool> m_streaming_started{false};  ///< 是否已输出 "● Thought for"（防重复）

    // 工具调用嵌套层级
    int m_tool_indent = 0;
};

} // namespace workx
