/**
 * @file anthropic_adapter.cpp
 * @brief Anthropic 协议适配器实现
 * @details Anthropic Messages API:
 *   - URL: POST /v1/messages
 *   - Auth: x-api-key header
 *   - Body: {model, max_tokens, stream, system?, messages}
 *   - SSE: named events (content_block_start/delta/stop, message_delta/stop)
 * @version 1.0.0
 * @date 2026-07
 */

#include "agent/api/provider/anthropic_adapter.h"

#ifdef WORKX_HAS_NLOHMANN_JSON
#include <nlohmann/json.hpp>
#endif

namespace agent {

std::string AnthropicAdapter::build_url(const std::string& base_url) const {
    std::string url = base_url;
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    url += "/v1/messages";
    return url;
}

std::vector<std::pair<std::string, std::string>> AnthropicAdapter::build_headers(
    const std::string& api_key) const {

    std::vector<std::pair<std::string, std::string>> headers;
    headers.emplace_back("Content-Type", "application/json");
    if (!api_key.empty()) {
        headers.emplace_back("x-api-key", api_key);
    }
    headers.emplace_back("anthropic-version", "2023-06-01");
    return headers;
}

std::string AnthropicAdapter::build_request_body(const CompletionRequest& request,
                                                  const std::string& model_name) const {
#ifdef WORKX_HAS_NLOHMANN_JSON
    nlohmann::json j;
    j["model"] = model_name;
    j["stream"] = request.stream;
    // Anthropic 的 max_tokens 为必填字段
    j["max_tokens"] = request.max_tokens > 0 ? request.max_tokens : 4096;

    // 分离 system prompt 和 messages
    // Anthropic API 要求 system 为顶层字段，messages 只含 user/assistant
    std::string system_prompt;
    nlohmann::json messages = nlohmann::json::array();

    for (const auto& msg : request.messages) {
        if (msg.role == ChatMessage::Role::System) {
            system_prompt = msg.content;  // 取最后一条 system
            continue;
        }

        nlohmann::json m;

        switch (msg.role) {
            case ChatMessage::Role::User:
                m["role"] = "user";
                m["content"] = msg.content;
                messages.push_back(m);
                break;

            case ChatMessage::Role::Assistant: {
                m["role"] = "assistant";
                // 有 tool_uses 时 content 为 content blocks 数组（text + tool_use）
                if (!msg.tool_uses.empty()) {
                    nlohmann::json content_blocks = nlohmann::json::array();
                    if (!msg.content.empty()) {
                        content_blocks.push_back({{"type", "text"}, {"text", msg.content}});
                    }
                    for (const auto& tu : msg.tool_uses) {
                        content_blocks.push_back({
                            {"type", "tool_use"},
                            {"id", tu.id},
                            {"name", tu.name},
                            {"input", tu.input.is_null() ? nlohmann::json::object() : tu.input}
                        });
                    }
                    m["content"] = std::move(content_blocks);
                } else {
                    if (!msg.reasoning_content.empty()) {
                        m["content"] = msg.reasoning_content + "\n" + msg.content;
                    } else {
                        m["content"] = msg.content;
                    }
                }
                messages.push_back(m);
                break;
            }

            case ChatMessage::Role::Tool:
                // Anthropic: tool_result 作为 user 消息的 content block
                m["role"] = "user";
                m["content"] = nlohmann::json::array({
                    {
                        {"type", "tool_result"},
                        {"tool_use_id", msg.tool_call_id},
                        {"content", msg.content}
                    }
                });
                messages.push_back(m);
                break;

            default:
                continue;
        }
    }

    if (!system_prompt.empty()) {
        j["system"] = system_prompt;
    }
    j["messages"] = std::move(messages);

    // 工具 schema（function calling）
    if (request.has_tools()) {
        j["tools"] = request.tools;
    }

    if (request.temperature >= 0) {
        j["temperature"] = request.temperature;
    }
    if (request.top_p > 0) {
        j["top_p"] = request.top_p;
    }
    if (!request.stop_words.empty()) {
        j["stop"] = request.stop_words;
    }

    return j.dump();
#else
    return "{\"model\":\"" + model_name + "\",\"stream\":true,\"messages\":[]}";
#endif
}

bool AnthropicAdapter::parse_sse_event(const std::string& event_type,
                                        const std::string& data,
                                        StreamChunk& out) const {
#ifdef WORKX_HAS_NLOHMANN_JSON
    try {
        auto json_obj = nlohmann::json::parse(data);

        if (event_type == "content_block_delta") {
            // 内容增量或 tool_use input 增量
            const auto& delta = json_obj["delta"];
            const std::string delta_type = delta.value("type", "");
            if (delta_type == "input_json_delta") {
                // tool_use input JSON 增量
                out.is_tool_use_delta = true;
                out.tool_input_delta = delta.value("partial_json", "");
                return true;
            }
            if (delta.contains("text") && !delta["text"].is_null()) {
                out.content_delta = delta["text"].get<std::string>();
                return true;
            }
        } else if (event_type == "content_block_start") {
            // 内容块开始：tool_use 或 text
            const auto& block = json_obj["content_block"];
            const std::string block_type = block.value("type", "text");
            if (block_type == "tool_use") {
                // tool_use 块开始
                out.is_tool_use_start = true;
                out.tool_use_id = block.value("id", "");
                out.tool_name = block.value("name", "");
                return true;
            }
            // 文本块可能有初始内容
            if (block.contains("text") && !block["text"].is_null()) {
                out.content_delta = block["text"].get<std::string>();
                return !out.content_delta.empty();
            }
        } else if (event_type == "message_delta") {
            // 消息完成增量：stop_reason + usage
            const auto& delta = json_obj["delta"];
            if (delta.contains("stop_reason") && !delta["stop_reason"].is_null()) {
                auto stop_reason = delta["stop_reason"].get<std::string>();
                if (stop_reason == "end_turn" || stop_reason == "max_tokens" ||
                    stop_reason == "stop_sequence" || stop_reason == "tool_use") {
                    out.is_final = true;
                    // usage 信息
                    if (json_obj.contains("usage") && !json_obj["usage"].is_null()) {
                        const auto& usage = json_obj["usage"];
                        out.generated_tokens = usage.value("output_tokens", 0);
                    }
                    return true;
                }
            }
        } else if (event_type == "message_stop") {
            // 消息停止标记
            out.is_final = true;
            return true;
        }

        // message_start, content_block_stop 等事件忽略
        return false;

    } catch (const nlohmann::json::parse_error&) {
        return false;
    } catch (const std::exception&) {
        return false;
    }
#else
    // 无 JSON 库时的简易回退
    if (event_type == "content_block_delta") {
        out.content_delta = data;
        return true;
    }
    return false;
#endif
}

} // namespace agent
