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
    subscribe_typed<agent::StreamTokenEvent>(
        [this](const agent::StreamTokenEvent& e) {
            if (!e.content_delta.empty()) push(ActionTokenDelta{e.content_delta});
            if (!e.reasoning_delta.empty()) push(ActionReasoningDelta{e.reasoning_delta});
        });

    subscribe_typed<agent::StreamDoneEvent>(
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
                .reasoning_ms = e.reasoning_ms,
            });
            push(ActionSetBusy{.busy = false});
        });

    subscribe_typed<agent::StepDoneEvent>(
        [this](const agent::StepDoneEvent&) { push(ActionStepDone{}); });

    subscribe_typed<agent::StreamErrorEvent>(
        [this](const agent::StreamErrorEvent& e) {
            push(ActionError{.message = e.message});
            push(ActionSetBusy{.busy = false});
        });

    subscribe_typed<agent::ToolCallEvent>(
        [this](const agent::ToolCallEvent& e) {
            push(ActionBeginTool{
                .tool_name = e.tool_name,
                .call_id = e.call_id,
                .arguments = e.arguments,
            });
        });

    subscribe_typed<agent::ToolResultEvent>(
        [this](const agent::ToolResultEvent& e) {
            push(ActionEndTool{
                .call_id = e.call_id,
                .result = e.result,
                .is_error = e.is_error,
            });
        });

    subscribe_typed<agent::AgentDoneEvent>(
        [this](const agent::AgentDoneEvent& e) {
            push(ActionAgentDone{
                .final_answer = e.final_response,
                .total_steps = e.total_steps,
                .total_tool_calls = e.total_tool_calls,
                .total_duration_ms = e.total_duration_ms,
            });
        });

    subscribe_typed<agent::AskUserRequestEvent>(
        [this](const agent::AskUserRequestEvent& e) {
            push(ActionAskUser{
                .questions = e.questions,
                .timeout_ms = e.timeout_ms,
                .result_promise = e.result_promise,
                .cancel_flag = e.cancel_flag,
            });
        });

    subscribe_typed<agent::AskUserTimeoutEvent>(
        [this](const agent::AskUserTimeoutEvent&) { push(ActionAskUserTimeout{}); });

    subscribe_typed<agent::EnterPlanModeEvent>(
        [this](const agent::EnterPlanModeEvent&) { push(ActionPermissions{.label = "plan"}); });

    subscribe_typed<agent::ExitPlanModeEvent>(
        [this](const agent::ExitPlanModeEvent&) { push(ActionPermissions{.label = ""}); });

    // B3：缓存诊断 / 压缩暂停 / 子 Agent 进度（对齐设计文档 §5 事件映射）
    subscribe_typed<agent::CacheDiagnosticsEvent>(
        [this](const agent::CacheDiagnosticsEvent& e) {
            push(ActionCacheDiagnostics{
                .prefix_hash = e.prefix_hash,
                .prefix_changed = e.prefix_changed,
                .reasons = e.reasons,
                .cache_hit_tokens = e.cache_hit_tokens,
                .cache_miss_tokens = e.cache_miss_tokens,
            });
        });

    subscribe_typed<agent::CompactionPausedEvent>(
        [this](const agent::CompactionPausedEvent& e) {
            push(ActionCompactionPaused{
                .paused = e.paused,
                .consecutive_compacts = e.consecutive_compacts,
                .notice = e.notice,
            });
        });

    subscribe_typed<agent::SubAgentProgressEvent>(
        [this](const agent::SubAgentProgressEvent& e) {
            push(ActionSubAgentProgress{
                .task_id = e.task_id,
                .step_number = e.step_number,
                .step_type = e.step_type,
                .content = e.content,
                // v1.3.0 结构化字段透传（第二层卡片渲染）
                .thought_text = e.thought_text,
                .tool_name = e.tool_name,
                .tool_input = e.tool_input,
                .observation = e.observation,
                .is_error = e.is_error,
                .duration_ms = e.duration_ms,
            });
        });

    subscribe_typed<agent::SubAgentCompletedEvent>(
        [this](const agent::SubAgentCompletedEvent& e) {
            push(ActionSubAgentCompleted{
                .task_id = e.task_id,
                .final_answer = e.final_answer,
                .was_error = e.was_error,
                .duration_ms = e.duration_ms,
            });
        });

    // #24：待办清单更新（TodoStore → 侧边栏 TODO 区块 / StatusBar 进度）
    subscribe_typed<agent::TodoUpdatedEvent>(
        [this](const agent::TodoUpdatedEvent& e) {
            push(ActionTodoUpdate{
                .session_id = e.session_id,
                .todos = e.todos,
            });
        });

    subscribe_typed<agent::ShutdownEvent>(
        [this](const agent::ShutdownEvent&) { push(ActionShutdown{}); });
}

void EventBridge::stop() {
    // B4：逐个精确退订（按订阅时登记的 token + 类型），不依赖外部 bus.clear() 兜底，
    //      也不误伤 EventBus 上其他订阅者。
    for (auto& unsub : m_unsubscribers) {
        if (unsub) unsub();
    }
    m_unsubscribers.clear();
}

}  // namespace ftxtui
