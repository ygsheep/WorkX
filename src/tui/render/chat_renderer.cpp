/**
 * @file chat_renderer.cpp
 * @brief 聊天渲染器实现
 * @details 包含 TUI 状态机、思考视图切换、结构化输出
 * @version 2.0.0
 */

#include "tui/render/chat_renderer.h"
#include "tui/core/terminal.h"
#include "tui/core/color_scheme.h"
#include "tui/widgets/status_bar.h"
#include "tui/render/spinner.h"
#include "tui/render/output_formatter.h"
#include "tui/render/streaming_buffer.h"
#include "core/events/event_bus.h"
#include "agent/message/types.h"

#include <chrono>
#include <thread>


namespace agent {

ChatRenderer::ChatRenderer(Terminal* terminal)
    : m_terminal(terminal)
    , m_status_bar(std::make_unique<StatusBar>(terminal))
    , m_formatter(std::make_unique<OutputFormatter>(terminal))
    , m_stream_buf(std::make_unique<StreamingBuffer>(terminal))
{
}

ChatRenderer::~ChatRenderer() {
    stop();
}

void ChatRenderer::start() {
    auto& bus = EventBus::instance();

    // 启动会话计时
    m_status_bar->start_session_timer();
    m_status_bar->subscribe_events();

    // ---- BackendStatusEvent → 状态转换 ----
    m_token_status = std::make_unique<EventToken>(
        bus.subscribe<BackendStatusEvent>([this](const BackendStatusEvent& e) {
            if (e.status == BackendStatusEvent::Connecting) {
                if (m_state_machine.current() == TuiState::IDLE) {
                    m_streaming_started = false;
                    transition_to(TuiState::THINKING);
                    m_thinking_start_time = std::chrono::steady_clock::now();
                    m_thinking_seconds = 0;
                    m_reasoning_buffer.clear();

                    // 启动 Spinner（思考计时）
                    m_terminal->spinner_start("Thinking");
                    m_spinner_active = true;

                    // 设置 Spinner 回调，只更新 StatusBar（差分渲染）
                    auto* spinner = m_terminal->get_spinner();
                    if (spinner) {
                        spinner->set_update_callback([this](int32_t seconds) {
                            m_thinking_seconds = seconds;
                            m_status_bar->set_thinking_seconds(seconds);
                            // 推进动画帧
                            m_status_bar->advance_frame();
                            // StatusBar 差分渲染
                            m_status_bar->render();
                        });
                    }

                    m_status_bar->set_state(TuiState::THINKING);
                    m_status_bar->render();
                }
            } else if (e.status == BackendStatusEvent::Connected) {
                if (m_spinner_active) {
                    m_terminal->spinner_stop();
                    m_spinner_active = false;
                }
            }
        })
    );

    // ---- StreamTokenEvent → 流式输出 ----
    m_token_stream = std::make_unique<EventToken>(
        bus.subscribe<StreamTokenEvent>([this](const StreamTokenEvent& e) {
            // 处理思考内容
            if (!e.reasoning_delta.empty()) {
                if (m_state_machine.current() != TuiState::THINKING) {
                    // 首次收到推理内容：输出 ● Thinking... 指示器
                    m_terminal->set_color(ColorRole::ThinkingIndicator);
                    m_terminal->write(" \xe2\x97\x8f Thinking... (ctrl+o to view)\n");
                    m_terminal->reset_color();

                    transition_to(TuiState::THINKING);
                    m_status_bar->set_state(TuiState::THINKING);
                    m_status_bar->render();
                }

                m_reasoning_buffer += e.reasoning_delta;

                if (m_viewing_thinking) {
                    m_terminal->set_color(ColorRole::Reasoning);
                    m_terminal->write(e.reasoning_delta);
                    m_terminal->reset_color();
                }
            }

            // 处理正文内容
            if (!e.content_delta.empty()) {
                if (!m_streaming_started.exchange(true)) {
                    // 第一次收到正文：切换到流式输出
                    if (!m_reasoning_buffer.empty()) {
                        m_terminal->set_color(ColorRole::Success);
                        m_terminal->write(" \xe2\x97\x8f Thought for ");
                        m_terminal->write(std::to_string(m_thinking_seconds));
                        m_terminal->write("s (ctrl+o to view)\n");
                        m_terminal->reset_color();
                    }

                    transition_to(TuiState::STREAMING);

                    m_stream_buf->start();
                    m_formatter->reset();
                }

                if (!m_viewing_thinking) {
                    m_formatter->feed(e.content_delta);
                }
            }

            // 更新 token 计数
            m_total_tokens += e.token_count;
            m_status_bar->set_token_count(m_total_tokens);
        })
    );

    // ---- StreamDoneEvent → 完成 ----
    m_token_done = std::make_unique<EventToken>(
        bus.subscribe<StreamDoneEvent>([this](const StreamDoneEvent& e) {
            if (m_spinner_active) {
                m_terminal->spinner_stop();
                m_spinner_active = false;
            }

            // 刷新 StreamingBuffer 和 OutputFormatter
            m_formatter->flush();
            m_stream_buf->stop();

            // 如果还在思考状态且有推理内容，输出 ● Thought for 标记
            if (m_state_machine.current() == TuiState::THINKING && !m_reasoning_buffer.empty()) {
                m_terminal->set_color(ColorRole::Success);
                m_terminal->write(" \xe2\x97\x8f Thought for ");  // ● (绿色)
                m_terminal->write(std::to_string(m_thinking_seconds));
                m_terminal->write("s (ctrl+o to view)\n");
                m_terminal->reset_color();
            }

            m_terminal->write("\n");

            // 显示 token 统计
            if (e.generated_tokens > 0) {
                double tok_per_s = e.generation_ms > 0
                    ? (e.generated_tokens / (e.generation_ms / 1000.0))
                    : 0.0;
                m_terminal->set_color(ColorRole::TokenStats);
                char stats[128];
                snprintf(stats, sizeof(stats), "%.0f tokens \xe2\x8b\x85 %.1f tok/s \xe2\x8b\x85 %.1fs\n",
                    static_cast<double>(e.generated_tokens), tok_per_s,
                    e.generation_ms / 1000.0);
                m_terminal->write(stats);
                m_terminal->reset_color();
                m_total_tokens += e.generated_tokens;
            }

            m_message_count++;
            m_terminal->mark_cursor_left_output();
            transition_to(TuiState::IDLE);
            m_status_bar->set_state(TuiState::IDLE);
            m_status_bar->set_token_count(m_total_tokens);
            m_status_bar->render();
            // 流结束，光标复位到输入行
            {
                int h = m_terminal->get_terminal_height();
                int input_row = h - 1;
                if (input_row < 1) input_row = 1;
                m_terminal->cursor_to_pos(input_row, 3);
            }
        })
    );

    // ---- StreamErrorEvent → 错误 ----
    m_token_error = std::make_unique<EventToken>(
        bus.subscribe<StreamErrorEvent>([this](const StreamErrorEvent& e) {
            if (m_spinner_active) {
                m_terminal->spinner_stop();
                m_spinner_active = false;
            }
            m_stream_buf->stop();
            m_formatter->reset();

            // 渲染错误块
            m_terminal->set_color(ColorRole::CodeBlock);
            m_terminal->write("\xe2\x94\x8c\xe2\x94\x80 Error ");  // ┌─ Error
            for (int i = 0; i < 40; ++i) m_terminal->write("\xe2\x94\x80");
            m_terminal->write("\xe2\x94\x90\n");  // ┐
            m_terminal->set_color(ColorRole::Error);
            m_terminal->write("\xe2\x94\x82 ");  // │
            m_terminal->write(e.message);
            m_terminal->write("\n");
            m_terminal->set_color(ColorRole::CodeBlock);
            m_terminal->write("\xe2\x94\x94");  // └
            for (int i = 0; i < 50; ++i) m_terminal->write("\xe2\x94\x80");
            m_terminal->write("\xe2\x94\x98\n");  // ┘
            m_terminal->reset_color();

            m_terminal->mark_cursor_left_output();
            // 将光标定位到输入行
            {
                int h = m_terminal->get_terminal_height();
                int input_row = h - 1;
                if (input_row < 1) input_row = 1;
                m_terminal->cursor_to_pos(input_row, 3);
            }
            transition_to(TuiState::ERROR);
            m_status_bar->set_state(TuiState::ERROR);
            m_status_bar->render();
        })
    );

    // ---- AgentStepEvent ----
    m_token_step = std::make_unique<EventToken>(
        bus.subscribe<AgentStepEvent>([this](const AgentStepEvent& e) {
            m_terminal->set_color(ColorRole::Bullet);
            m_terminal->write(std::format("  \xe2\x97\x8c Step {}: {}\n", e.step_number, e.description));
            m_terminal->reset_color();
        })
    );

    // ---- ToolCallEvent ----
    m_token_tool_call = std::make_unique<EventToken>(
        bus.subscribe<ToolCallEvent>([this](const ToolCallEvent& e) {
            auto icon = get_tool_icon(e.tool_name);
            std::string indent(m_tool_indent * 2, ' ');

            m_terminal->set_color(ColorRole::ToolName);
            m_terminal->write(std::format("{}{} {} (ctrl+o to view)\n",
                indent, icon.icon, e.tool_name));
            m_terminal->reset_color();

            // 更新 StatusBar
            m_status_bar->set_tool_name(e.tool_name);
            transition_to(TuiState::TOOL_RUNNING);
            m_status_bar->set_state(TuiState::TOOL_RUNNING);
            m_status_bar->render();

            m_tool_indent++;
        })
    );

    // ---- ToolResultEvent ----
    m_token_tool_result = std::make_unique<EventToken>(
        bus.subscribe<ToolResultEvent>([this](const ToolResultEvent& e) {
            m_tool_indent = m_tool_indent > 0 ? m_tool_indent - 1 : 0;
            std::string indent(m_tool_indent * 2, ' ');

            // 成功/失败标记
            const char* marker = e.is_error
                ? "\xe2\x9c\x97"  // ✗
                : "\xe2\x9c\x93"; // ✓
            ColorRole marker_color = e.is_error ? ColorRole::Failure : ColorRole::Success;

            // 结果摘要
            std::string preview = e.result;
            if (preview.length() > 200) {
                preview = preview.substr(0, 200) + "...";
            }

            m_terminal->set_color(marker_color);
            m_terminal->write(indent + "  ");
            m_terminal->write(marker);
            m_terminal->set_color(ColorRole::ToolOutput);
            m_terminal->write(" \xe2\x8e\xbf  " + preview + "\n");  // ⎿
            m_terminal->reset_color();

            transition_to(TuiState::STREAMING);
            m_status_bar->set_state(TuiState::STREAMING);
            m_status_bar->render();
        })
    );

    // ---- AgentDoneEvent ----
    m_token_agent_done = std::make_unique<EventToken>(
        bus.subscribe<AgentDoneEvent>([this](const AgentDoneEvent& e) {
            m_terminal->set_color(ColorRole::Success);
            m_terminal->write(std::format("  \xe2\x9c\x93 Agent done: {} steps, {} tool calls\n",
                e.total_steps, e.total_tool_calls));
            m_terminal->reset_color();
            m_tool_indent = 0;
        })
    );
}

void ChatRenderer::stop() {
    auto& bus = EventBus::instance();

    if (m_spinner_active) {
        m_terminal->spinner_stop();
        m_spinner_active = false;
    }
    m_stream_buf->stop();
    m_status_bar->unsubscribe_events();

    if (m_token_status && m_token_status->is_valid()) {
        bus.unsubscribe<BackendStatusEvent>(*m_token_status);
    }
    if (m_token_stream && m_token_stream->is_valid()) {
        bus.unsubscribe<StreamTokenEvent>(*m_token_stream);
    }
    if (m_token_done && m_token_done->is_valid()) {
        bus.unsubscribe<StreamDoneEvent>(*m_token_done);
    }
    if (m_token_error && m_token_error->is_valid()) {
        bus.unsubscribe<StreamErrorEvent>(*m_token_error);
    }
    if (m_token_step && m_token_step->is_valid()) {
        bus.unsubscribe<AgentStepEvent>(*m_token_step);
    }
    if (m_token_tool_call && m_token_tool_call->is_valid()) {
        bus.unsubscribe<ToolCallEvent>(*m_token_tool_call);
    }
    if (m_token_tool_result && m_token_tool_result->is_valid()) {
        bus.unsubscribe<ToolResultEvent>(*m_token_tool_result);
    }
    if (m_token_agent_done && m_token_agent_done->is_valid()) {
        bus.unsubscribe<AgentDoneEvent>(*m_token_agent_done);
    }
}

void ChatRenderer::transition_to(TuiState new_state) {
    m_state_machine.transition_to(new_state);
    if (m_state_machine.current() != new_state) {
        m_state_machine.force_state(new_state);
    }
}

void ChatRenderer::toggle_thinking_view() {
    if (m_reasoning_buffer.empty()) return;

    if (!m_viewing_thinking) {
        // ---- 展开思考视图：清屏 + 打字机效果渲染 ----
        m_viewing_thinking = true;
        m_terminal->write("\x1b[2J\x1b[H");

        // 渲染思考标题框
        m_terminal->set_color(ColorRole::ThinkingBlock);
        m_terminal->write("\xe2\x94\x8c\xe2\x94\x80 Thought for ");
        m_terminal->write(std::to_string(m_thinking_seconds));
        m_terminal->write("s (ctrl+o to return) ");
        m_terminal->write("\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x80\xe2\x94\x90\n");
        m_terminal->reset_color();

        // 打字机效果渲染思考内容
        m_terminal->set_color(ColorRole::Reasoning);
        for (size_t i = 0; i < m_reasoning_buffer.size(); ++i) {
            char c = m_reasoning_buffer[i];
            m_terminal->write(std::string(1, c));
            if (c != ' ' && c != '\n' && c != '\r') {
                std::this_thread::sleep_for(std::chrono::milliseconds(15));
            }
        }
        m_terminal->reset_color();

        // 底部边框
        m_terminal->write("\n");
        m_terminal->set_color(ColorRole::ThinkingBlock);
        m_terminal->write("\xe2\x94\x94");
        for (int i = 0; i < 50; ++i) m_terminal->write("\xe2\x94\x80");
        m_terminal->write("\xe2\x94\x98\n");
        m_terminal->set_color(ColorRole::Dim);
        m_terminal->write("  (ctrl+o to return)\n");
        m_terminal->reset_color();
    } else {
        // ---- 收起思考视图：清屏 + 重新渲染对话摘要 ----
        m_viewing_thinking = false;
        m_terminal->write("\x1b[2J\x1b[H");

        m_terminal->set_color(ColorRole::Success);
        m_terminal->write(std::format(" \xe2\x97\x8f Thought for {}s (ctrl+o to view)\n", m_thinking_seconds));
        m_terminal->reset_color();

        if (m_state_machine.current() == TuiState::STREAMING) {
            m_terminal->set_color(ColorRole::Dim);
            m_terminal->write("  [streaming in progress...]\n");
            m_terminal->reset_color();
        }

        m_status_bar->render();
    }
}

} // namespace workx
