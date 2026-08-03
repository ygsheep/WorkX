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
#include <vector>

#include "tui/core/tui_state.h"
#include "tui/model/token_stats_model.h"
#include "tui/model/tool_call_tracker.h"

namespace agent { class EventToken; }
namespace agent { struct ChatMessage; }

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

    /// @brief 重绘历史消息到输出区域（会话恢复 / /resume 切换后调用）
    /// @details 清屏并遍历消息历史，按角色渲染：
    ///          - User: "> 内容"（Prompt 色）
    ///          - Assistant: 通过 OutputFormatter 渲染 markdown
    ///          - Tool: 工具调用标记 + 结果摘要
    ///          调用前需确保已退出 overlay（如 SessionPicker），scroll region 已恢复。
    /// @param show_welcome 是否在历史消息前渲染欢迎横幅（启动恢复时为 true，/resume 切换时为 false）
    void replay_history(const std::vector<agent::ChatMessage>& messages, bool show_welcome = false);

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
    std::unique_ptr<agent::EventToken> m_token_resize;      ///< 终端尺寸变更事件
    std::unique_ptr<agent::EventToken> m_token_task_completed;  ///< 后台任务完成事件
    std::unique_ptr<agent::EventToken> m_token_task_failed;     ///< 后台任务失败事件
    std::unique_ptr<agent::EventToken> m_token_cache_diag;      ///< 缓存诊断事件
    std::unique_ptr<agent::EventToken> m_token_ask_user;        ///< AskUser 请求事件
    std::unique_ptr<agent::EventToken> m_token_ask_timeout;     ///< AskUser 超时事件

    // 状态机
    TuiStateMachine m_state_machine;

    // 思考内容管理
    // T-3：跨线程字段原子化（事件回调在 main loop，toggle_thinking_view 在键盘中断，
    //      Spinner 回调在 spinner 线程，需保证可见性与原子性）
    std::atomic<bool> m_spinner_active{false};
    std::string m_reasoning_buffer;          ///< 缓存的思考内容（仅在 main loop 访问，单线程）
    std::chrono::steady_clock::time_point m_thinking_start_time;
    // M-1: 移除 m_viewing_thinking，统一通过 Terminal::is_overlay_active() 查询 overlay 状态
    // 消除 ChatRenderer 与 Terminal 间状态非原子窗口
    std::atomic<int32_t> m_thinking_seconds{0};          ///< 思考持续秒数（Spinner 线程写）

    // H-1 修复：overlay 期间缓冲流式内容，收起时统一 flush 到 formatter
    // 避免流式输出直接写入终端破坏思考视图显示，且收起后内容丢失
    std::string m_pending_content;            ///< overlay 期间累积的正文内容（单线程访问）

    // P2: 提取的状态管理模型
    TokenStatsModel m_token_stats;            ///< 会话 token 统计
    ToolCallTracker m_tool_tracker;           ///< 工具调用嵌套与上下文管理
    std::atomic<bool> m_streaming_started{false};  ///< 是否已输出 "● Thought for"（防重复）
    bool m_thinking_indicator_shown = false;  ///< 是否已输出 "● 思考中..." 临时候选标记（供覆盖）

    // ---- ctrl+o 就地展开（局部 overlay）----
    // 思考结束后标记 "● 思考 Ns" 已输出到对话流，ctrl+o 时只对标记行下方区域
    // 做 overlay（快照+恢复），保留标记行上方的对话内容。
    // 比旧方案（覆盖整个对话区）保留更多上下文。
    int m_thinking_marker_physical_row = 0;  ///< "● 思考 Ns" 标记的物理行号（1-based）
    int m_thinking_marker_offset = 0;        ///< DisplayBuffer 与终端的行偏差修正（"思考中..."覆盖导致）
    bool m_thinking_expanded = false;        ///< 思考内容是否已就地展开
    bool m_thinking_used_full_overlay = false; ///< 标记滚出屏幕时 fallback 到全屏 overlay
};

} // namespace tui
