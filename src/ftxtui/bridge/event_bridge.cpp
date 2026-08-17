#include "bridge/event_bridge.h"

#include <utility>

#include "core/events/agent_events.h"
#include "core/events/stream_events.h"
#include "core/events/system_events.h"

namespace ftxtui {

void EventBridge::push(Action action) {
    m_queue.push(std::move(action));
    if (m_wake) m_wake();
}

void EventBridge::start() {
    auto& bus = m_bus;

    m_tokens.push_back(bus.subscribe<agent::StreamTokenEvent>(
        [this](const agent::StreamTokenEvent& e) {
            if (!e.content_delta.empty()) push(ActionTokenDelta{e.content_delta});
            if (!e.reasoning_delta.empty()) push(ActionReasoningDelta{e.reasoning_delta});
        }));

    m_tokens.push_back(bus.subscribe<agent::StreamDoneEvent>(
        [this](const agent::StreamDoneEvent& e) {
            push(ActionTurnDone{
                .full_content = e.full_content,
                .full_reasoning = e.full_reasoning,
                .was_interrupted = e.was_interrupted,
                .is_local_command = e.is_local_command,
                .prompt_tokens = e.prompt_tokens,
                .generated_tokens = e.generated_tokens,
                .cache_read_input_tokens = e.cache_read_input_tokens,
                .prompt_ms = e.prompt_ms,
                .generation_ms = e.generation_ms,
            });
            push(ActionSetBusy{.busy = false});
        }));

    m_tokens.push_back(bus.subscribe<agent::StepDoneEvent>(
        [this](const agent::StepDoneEvent&) { push(ActionStepDone{}); }));

    m_tokens.push_back(bus.subscribe<agent::StreamErrorEvent>(
        [this](const agent::StreamErrorEvent& e) {
            push(ActionError{.message = e.message});
            push(ActionSetBusy{.busy = false});
        }));

    m_tokens.push_back(bus.subscribe<agent::ToolCallEvent>(
        [this](const agent::ToolCallEvent& e) {
            push(ActionBeginTool{
                .tool_name = e.tool_name,
                .call_id = e.call_id,
                .arguments = e.arguments,
            });
        }));

    m_tokens.push_back(bus.subscribe<agent::ToolResultEvent>(
        [this](const agent::ToolResultEvent& e) {
            push(ActionEndTool{
                .call_id = e.call_id,
                .result = e.result,
                .is_error = e.is_error,
            });
        }));

    m_tokens.push_back(bus.subscribe<agent::AgentDoneEvent>(
        [this](const agent::AgentDoneEvent& e) {
            push(ActionAgentDone{
                .final_answer = e.final_response,
                .total_steps = e.total_steps,
                .total_tool_calls = e.total_tool_calls,
                .total_duration_ms = e.total_duration_ms,
            });
        }));

    m_tokens.push_back(bus.subscribe<agent::AskUserRequestEvent>(
        [this](const agent::AskUserRequestEvent& e) {
            push(ActionAskUser{
                .questions = e.questions,
                .timeout_ms = e.timeout_ms,
                .result_promise = e.result_promise,
                .cancel_flag = e.cancel_flag,
            });
        }));

    m_tokens.push_back(bus.subscribe<agent::AskUserTimeoutEvent>(
        [this](const agent::AskUserTimeoutEvent&) { push(ActionAskUserTimeout{}); }));

    m_tokens.push_back(bus.subscribe<agent::EnterPlanModeEvent>(
        [this](const agent::EnterPlanModeEvent&) { push(ActionPermissions{.label = "plan"}); }));

    m_tokens.push_back(bus.subscribe<agent::ExitPlanModeEvent>(
        [this](const agent::ExitPlanModeEvent&) { push(ActionPermissions{.label = ""}); }));

    m_tokens.push_back(bus.subscribe<agent::ShutdownEvent>(
        [this](const agent::ShutdownEvent&) { push(ActionShutdown{}); }));
}

void EventBridge::stop() {
    auto& bus = m_bus;
    for (auto& t : m_tokens) {
        // 统一通过泛型不可行；这里逐事件退订。简化：EventBus 支持 clear()，
        // 但会误伤其他订阅。为最小侵入，仅记录并提供 clear 供外部决定。
        (void)t;
    }
    // 注意：IEventBus 不暴露按 token 的通用退订；此实验 UI 进程独享 EventBus，
    //       退出时由外部调用 bus.clear()（见 app 的清理顺序）。
    m_tokens.clear();
}

}  // namespace ftxtui