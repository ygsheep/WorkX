#include "vm/view_model.h"

#include <format>
#include <string>
#include <utility>

#include "theme/strings.h"

namespace ftxtui {

MessageNode& ViewModel::active_stream() {
    if (!messages.empty()) {
        auto& back = messages.back();
        if (back.role == MsgRole::Assistant && !back.sealed) {
            return back;
        }
    }
    messages.push_back(MessageNode{});
    return messages.back();
}

bool ViewModel::has_active_stream() const {
    if (messages.empty()) return false;
    const auto& back = messages.back();
    return back.role == MsgRole::Assistant && !back.sealed;
}

bool ViewModel::apply(const Action& action) {
    return std::visit([this](const auto& a) -> bool { return apply_variant(a); }, action);
}

// 分派单个 action（private helper，经由 std::visit 调用）
bool ViewModel::apply_variant(const ActionAppendMessage& a) {
    MessageNode n;
    n.role = a.role == "user" ? MsgRole::User : MsgRole::Assistant;
    n.text = a.text;
    n.sealed = true;
    messages.push_back(std::move(n));
    return true;
}

bool ViewModel::apply_variant(const ActionTokenDelta& a) {
    auto& m = active_stream();
    m.streaming = true;
    m.text.append(a.content_delta);
    return true;
}

bool ViewModel::apply_variant(const ActionReasoningDelta& a) {
    auto& m = active_stream();
    if (!a.delta.empty()) {
        if (!m.reasoned) {
            m.reasoned = true;
            m.reasoning_expanded = card_defaults.reasoning_expanded;
        }
    }
    m.reasoning.append(a.delta);
    return true;
}

bool ViewModel::apply_variant(const ActionStepDone&) {
    // 单步结束但仍有工具待执行：不封口，仅停止流式标记
    if (!messages.empty() && messages.back().role == MsgRole::Assistant) {
        messages.back().streaming = false;
    }
    return true;
}

bool ViewModel::apply_variant(const ActionTurnDone& a) {
    auto& m = active_stream();
    m.streaming = false;
    m.sealed = true;
    if (!a.full_content.empty() && m.text.empty()) m.text = a.full_content;
    if (!a.full_reasoning.empty() && m.reasoning.empty()) m.reasoning = a.full_reasoning;
    m.prompt_tokens = a.prompt_tokens;
    m.generated_tokens = a.generated_tokens;
    m.cache_read_tokens = a.cache_read_input_tokens;
    m.duration_ms = a.prompt_ms + a.generation_ms;
    m.reasoning_ms = a.reasoning_ms;
    total_tokens = a.prompt_tokens + a.generated_tokens + a.cache_read_input_tokens;
    sidebar.context_used = a.prompt_tokens;
    sidebar.cache_read_tokens = a.cache_read_input_tokens;
    sidebar.total_tokens = total_tokens;
    busy = false;
    return true;
}

bool ViewModel::apply_variant(const ActionError& a) {
    MessageNode n;
    n.role = MsgRole::Error;
    n.text = a.message;
    n.sealed = true;
    messages.push_back(std::move(n));
    busy = false;
    return true;
}

bool ViewModel::apply_variant(const ActionBeginTool& a) {
    auto& m = active_stream();
    if (m.find_tool(a.call_id)) return false;  // 去重
    ToolCallNode t;
    t.tool_name = a.tool_name;
    t.call_id = a.call_id;
    t.arguments = a.arguments;
    t.running = true;
    t.expanded = card_defaults.tool_expanded;
    t.text_pos = m.text.size();  // 记录正文插入点，供与正文交错渲染
    m.tool_calls.push_back(std::move(t));
    m.tool_use_ids.push_back(a.call_id);
    return true;
}

bool ViewModel::apply_variant(const ActionEndTool& a) {
    // 在最后一条 assistant（可为已封口）里找；找不到则全表找
    for (size_t i = messages.size(); i-- > 0;) {
        auto& m = messages[i];
        if (auto* t = m.find_tool(a.call_id)) {
            t->result = a.result;
            t->done = true;
            t->running = false;
            t->is_error = a.is_error;
            t->expanded = a.is_error;  // 出错默认展开
            return true;
        }
    }
    return false;
}

bool ViewModel::apply_variant(const ActionAgentDone& a) {
    busy = false;
    // 成功路径 StreamDoneEvent（ActionTurnDone）已先行封口并填充 final_answer
    //（同一线程顺序发布，队列保序），AgentDoneEvent 只是最终汇总：
    // 仅当流式路径缺失（事件丢失/顺序异常）且消息未封口时才补填，
    // 绝不追加新消息——否则 final_answer 会作为第二遍重复显示。
    if (!messages.empty() && messages.back().role == MsgRole::Assistant
        && !messages.back().sealed) {
        auto& m = messages.back();
        if (m.text.empty() && !a.final_answer.empty()) m.text = a.final_answer;
        m.sealed = true;
        m.streaming = false;
        m.duration_ms = a.total_duration_ms;
    }
    return true;
}

bool ViewModel::apply_variant(const ActionSetBusy& a) {
    busy = a.busy;
    if (a.busy) {
        // 为即将到来的流式 token 预留一条 assistant 消息
        (void)active_stream();
        messages.back().streaming = false;
    }
    return true;
}

bool ViewModel::apply_variant(const ActionPermissions& a) {
    if (sidebar.permission == a.label) return false;
    sidebar.permission = a.label;
    return true;
}

bool ViewModel::apply_variant(const ActionAskUser&) { return true; }
bool ViewModel::apply_variant(const ActionAskUserTimeout&) { return true; }

bool ViewModel::apply_variant(const ActionCacheDiagnostics& a) {
    // 仅 prefix_changed 时提示（对齐 src/tui ChatRenderer 语义）
    if (!a.prefix_changed) return false;
    std::string reason_str;
    for (size_t i = 0; i < a.reasons.size(); ++i) {
        if (i > 0) reason_str += "+";
        reason_str += a.reasons[i];
    }
    prompt_echo = std::string(str::kCachePrefixChanged)
                  + (reason_str.empty() ? "?" : reason_str)
                  + std::string(str::kCacheMissSep)
                  + std::to_string(a.cache_miss_tokens)
                  + std::string(str::kCacheTokensUnit);
    return true;
}

bool ViewModel::apply_variant(const ActionCompactionPaused& a) {
    if (!a.notice.empty()) prompt_echo = a.notice;
    else if (a.paused) prompt_echo = std::string(str::kCompactPausedPrefix)
                                        + std::to_string(a.consecutive_compacts)
                                        + std::string(str::kCompactPausedSuffix);
    else prompt_echo = std::string(str::kCompactResumed);
    return true;
}

bool ViewModel::apply_variant(const ActionSubAgentProgress& a) {
    // 子任务进度：追加一条 assistant 消息（含 task_id 标识），不流式
    std::string prefix = std::string(str::kSubAgentPrefix) + a.task_id + std::string(str::kSubAgentSep);
    if (a.step_type == "final") {
        // final 步由 SubAgentCompleted 处理，这里不重复
        return false;
    }
    MessageNode n;
    n.role = MsgRole::Assistant;
    n.text = prefix + (a.content.empty()
        ? (std::string(str::kSubStepPrefix) + std::to_string(a.step_number)
           + std::string(str::kSubStepOpen) + a.step_type + std::string(str::kSubStepClose))
        : a.content);
    n.sealed = true;
    messages.push_back(std::move(n));
    return true;
}

bool ViewModel::apply_variant(const ActionSubAgentCompleted& a) {
    MessageNode n;
    n.role = MsgRole::Assistant;
    std::string status = a.was_error ? std::string(str::kSubFailed) : std::string(str::kSubCompleted);
    n.text = std::string(str::kSubAgentPrefix) + a.task_id + std::string(str::kSubAgentSep) + status
             + std::format(str::kSubDuration, a.duration_ms / 1000.0);
    if (!a.final_answer.empty()) {
        n.text += "\n" + a.final_answer;
    }
    n.sealed = true;
    messages.push_back(std::move(n));
    return true;
}

bool ViewModel::apply_variant(const ActionShutdown&) {
    pending_exit = true;
    return true;
}

bool ViewModel::apply_variant(const ActionToast& a) {
    prompt_echo = a.text;
    return true;
}

bool ViewModel::apply_variant(const ActionModelsLoaded&) {
    return false;  // 由 App 消费（刷新模型列表），ViewModel 不关心
}

bool ViewModel::apply_variant(const ActionSessionsLoaded&) {
    return false;  // 由 App 消费（填充会话搜索条目），ViewModel 不关心
}

bool ViewModel::apply_variant(const ActionProviderSwitched&) {
    return false;  // 由 App 消费（运行时热切换后端），ViewModel 不关心
}

bool ViewModel::apply_variant(const ActionProviderSwitchFailed&) {
    return false;  // 由 App 消费（提示切换失败），ViewModel 不关心
}

}  // namespace ftxtui