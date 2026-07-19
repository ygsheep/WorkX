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
#include <cstdint>

#include "tui/core/tui_state.h"
#include "tui/render/session_log.h"

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

    /// @brief 切换思考视图（ctrl+o）—— 兼容旧接口，转发到 toggle_detail_view
    void toggle_thinking_view();

    /// @brief 切换详情视图（ctrl+o）
    void toggle_detail_view();

    /// @brief 详情视图中的按键处理（滚动/退出）
    /// @return true 表示已处理该按键，false 表示未处理（仅当按 Ctrl+O 退出时返回 false）
    bool handle_detail_input(char32_t key);

    /// @brief 当前是否处于详情视图
    bool is_detail_view_active() const { return m_detail_view_active; }

    /// @brief 获取当前 TUI 状态
    TuiState state() const { return m_state_machine.current(); }

    /// @brief 获取 StatusBar（供 Terminal 使用）
    StatusBar* status_bar() const { return m_status_bar.get(); }

private:
    void transition_to(TuiState new_state);

    /// @brief 渲染详情视图（按 m_scroll_offset 滚动）
    void render_detail_view();

    /// @brief 收起详情视图，重渲对话流
    void collapse_detail_view();

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
    std::string m_reasoning_buffer;          ///< 缓存的思考内容（当前轮）
    std::chrono::steady_clock::time_point m_thinking_start_time;
    bool m_viewing_thinking = false;         ///< 是否在思考视图（旧字段，保留）
    int32_t m_thinking_seconds = 0;          ///< 思考持续秒数

    // 会话统计
    int32_t m_message_count = 0;
    int32_t m_total_tokens = 0;
    std::atomic<bool> m_streaming_started{false};  ///< 是否已输出 "● Thought for"（防重复）

    // 工具调用嵌套层级
    int m_tool_indent = 0;

    // 最近一次工具调用（供 ToolResultEvent 渲染时区分工具类型）
    std::string m_last_tool_name;
    std::string m_last_tool_arguments;

    // ---- 详情视图（Ctrl+O）----
    SessionLog m_session_log;                ///< 本轮 Agent 编排的完整日志
    bool m_detail_view_active = false;       ///< 是否处于详情视图
    int m_scroll_offset = 0;                 ///< 滚动偏移（行数，0=顶部）

    // 当前 Thought 流式累积缓冲（用于在 on_step 时存入 log）
    std::string m_current_reasoning;         ///< 当前 Thought 的 reasoning 累积
    std::string m_current_content;           ///< 当前 Thought 的 content 累积
    int32_t m_current_step_number = 0;       ///< 当前 Thought 的步骤号
};

} // namespace agent
