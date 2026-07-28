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
#include "tui/model/token_stats_model.h"
#include "tui/model/tool_call_tracker.h"

namespace agent { class EventToken; }

namespace tui {

class Terminal;
class StatusBar;
class Spinner;
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

    /// E.9：输入提示列位置常量（流结束/错误时光标复位的列）
    static constexpr int INPUT_PROMPT_COL = 3;

    Terminal* m_terminal;
    std::unique_ptr<StatusBar> m_status_bar;
    std::unique_ptr<OutputFormatter> m_formatter;
    std::unique_ptr<StreamingBuffer> m_stream_buf;

    // 事件订阅 token
    std::unique_ptr<agent::EventToken> m_token_status;
    std::unique_ptr<agent::EventToken> m_token_stream;
    std::unique_ptr<agent::EventToken> m_token_done;
    std::unique_ptr<agent::EventToken> m_token_step_done;  ///< P3: 单步结束（ReAct 中间步骤）
    std::unique_ptr<agent::EventToken> m_token_error;
    std::unique_ptr<agent::EventToken> m_token_step;
    std::unique_ptr<agent::EventToken> m_token_tool_call;
    std::unique_ptr<agent::EventToken> m_token_tool_result;
    std::unique_ptr<agent::EventToken> m_token_agent_done;
    std::unique_ptr<agent::EventToken> m_token_user_input;  ///< 用户输入事件（用于本地 token 估算）

    // 状态机
    TuiStateMachine m_state_machine;

    // 思考内容管理
    // T-3：跨线程字段原子化（事件回调在 main loop，toggle_thinking_view 在键盘中断，
    //      Spinner 回调在 spinner 线程，需保证可见性与原子性）
    std::atomic<bool> m_spinner_active{false};
    std::string m_reasoning_buffer;          ///< 缓存的思考内容（仅在 main loop 访问，单线程）
    std::chrono::steady_clock::time_point m_thinking_start_time;
    std::atomic<bool> m_viewing_thinking{false};         ///< 是否在思考视图
    std::atomic<int32_t> m_thinking_seconds{0};          ///< 思考持续秒数（Spinner 线程写）

    // H-1 修复：overlay 期间缓冲流式内容，收起时统一 flush 到 formatter
    // 避免流式输出直接写入终端破坏思考视图显示，且收起后内容丢失
    std::string m_pending_content;            ///< overlay 期间累积的正文内容（单线程访问）

    // P2: 提取的状态管理模型
    TokenStatsModel m_token_stats;            ///< 会话 token 统计
    ToolCallTracker m_tool_tracker;           ///< 工具调用嵌套与上下文管理
    std::atomic<bool> m_streaming_started{false};  ///< 是否已输出 "● Thought for"（防重复）
};

} // namespace tui
