#include "event_bridge.h"

#include <functional>
#include <nlohmann/json.hpp>

#include "core/events/agent_events.h"
#include "core/events/i_event_bus.h"
#include "core/events/system_events.h"

namespace di {

namespace {

/// 截断到 max_chars（按 UTF-8 码点，避免截断多字节字符）
std::string truncate(const std::string& s, size_t max_chars) {
    if (s.size() <= max_chars) return s;
    size_t cut = max_chars;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
    return s.substr(0, cut) + "...";
}

std::string strip_newlines(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\n' || c == '\r') out += ' ';
        else out += c;
    }
    return out;
}

} // namespace

EventBridge::EventBridge(agent::IEventBus& bus) : m_bus(bus) {
    subscribe<agent::ToolCallEvent>(
        [this](const agent::ToolCallEvent& e) {
            enqueue(NotifyKind::Tool, "工具调用: " + e.tool_name,
                    truncate(strip_newlines(e.arguments), 96));
        });
    subscribe<agent::ToolResultEvent>(
        [this](const agent::ToolResultEvent& e) {
            if (e.is_error) {
                enqueue(NotifyKind::Error, "工具失败: " + e.call_id,
                        truncate(strip_newlines(e.result), 96));
            } else {
                enqueue(NotifyKind::Success, "工具完成: " + e.call_id,
                        truncate(strip_newlines(e.result), 96));
            }
        });
    subscribe<agent::AgentDoneEvent>(
        [this](const agent::AgentDoneEvent& e) {
            std::string body = truncate(strip_newlines(e.final_response), 96);
            body += "  (" + std::to_string(e.total_steps) + " 步 / " +
                    std::to_string(e.total_tool_calls) + " 次工具)";
            enqueue(NotifyKind::Success, "任务完成", body);
        });
    subscribe<agent::AskUserRequestEvent>(
        [this](const agent::AskUserRequestEvent&) {
            enqueue(NotifyKind::Warning, "需要你的确认",
                    "Agent 正在等待交互，请切回终端查看");
        });
    subscribe<agent::BackendStatusEvent>(
        [this](const agent::BackendStatusEvent& e) {
            const char* state = "未知";
            NotifyKind kind = NotifyKind::Info;
            switch (e.status) {
            case agent::BackendStatusEvent::Connecting: state = "连接中"; break;
            case agent::BackendStatusEvent::Connected:  state = "已连接"; kind = NotifyKind::Success; break;
            case agent::BackendStatusEvent::Error:      state = "连接失败"; kind = NotifyKind::Error; break;
            case agent::BackendStatusEvent::Disconnected: state = "已断开"; kind = NotifyKind::Warning; break;
            }
            enqueue(kind, "后端: " + e.backend_name, state);
        });
    subscribe<agent::CompactionPausedEvent>(
        [this](const agent::CompactionPausedEvent& e) {
            if (e.paused) {
                enqueue(NotifyKind::Warning, "上下文压缩已暂停", e.notice);
            } else {
                enqueue(NotifyKind::Success, "上下文压缩已恢复", e.notice);
            }
        });
}

void EventBridge::enqueue(NotifyKind kind, std::string title, std::string body) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_queue.push_back(IslandMessage{kind, std::move(title), std::move(body)});
}

void EventBridge::Drain(std::vector<IslandMessage>& out) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_queue.empty()) return;
    out.insert(out.end(), m_queue.begin(), m_queue.end());
    m_queue.clear();
}

} // namespace di
