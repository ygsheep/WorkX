/**
 * @file openai_adapter.cpp
 * @brief OpenAI 协议适配器实现
 * @details 从原有的 RemoteBackend 提取，封装为独立适配器
 * @version 1.0.0
 * @date 2026-07
 */

#include "agent/api/provider/openai_adapter.h"

#ifdef WORKX_HAS_NLOHMANN_JSON
#include <nlohmann/json.hpp>
#endif

namespace agent {

std::string OpenAIAdapter::build_url(const std::string& base_url) const {
    std::string url = base_url;
    // 去重尾部 /
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    url += "/v1/chat/completions";
    return url;
}

std::vector<std::pair<std::string, std::string>> OpenAIAdapter::build_headers(
    const std::string& api_key) const {

    std::vector<std::pair<std::string, std::string>> headers;
    headers.emplace_back("Content-Type", "application/json");
    if (!api_key.empty()) {
        headers.emplace_back("Authorization", "Bearer " + api_key);
    }
    return headers;
}

std::string OpenAIAdapter::build_request_body(const CompletionRequest& request,
                                               const std::string& model_name) const {
#ifdef WORKX_HAS_NLOHMANN_JSON
    nlohmann::json j;
    j["model"] = model_name;
    j["stream"] = request.stream;

    auto& messages = j["messages"];
    for (const auto& msg : request.messages) {
        nlohmann::json m;
        switch (msg.role) {
            case ChatMessage::Role::System:    m["role"] = "system"; break;
            case ChatMessage::Role::User:      m["role"] = "user"; break;
            case ChatMessage::Role::Assistant: m["role"] = "assistant"; break;
            case ChatMessage::Role::Tool:      m["role"] = "tool"; break;
        }
        m["content"] = msg.content;
        if (!msg.reasoning_content.empty()) {
            m["reasoning_content"] = msg.reasoning_content;
        }
        if (msg.role == ChatMessage::Role::Tool) {
            m["tool_call_id"] = msg.tool_call_id;
        }
        messages.push_back(m);
    }

    if (request.max_tokens > 0) {
        j["max_tokens"] = request.max_tokens;
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

bool OpenAIAdapter::parse_sse_event(const std::string& /*event_type*/,
                                     const std::string& data,
                                     StreamChunk& out) const {
    // OpenAI [DONE] 标记
    if (data == "[DONE]") {
        out.is_final = true;
        return true;
    }

#ifdef WORKX_HAS_NLOHMANN_JSON
    try {
        auto json_obj = nlohmann::json::parse(data);

        // 检查错误
        if (json_obj.contains("error")) {
            return false;
        }

        // 解析 choices[0].delta
        if (!json_obj.contains("choices") || json_obj["choices"].empty()) {
            return false;
        }

        const auto& choice = json_obj["choices"][0];
        const auto& delta = choice.value("delta", nlohmann::json::object());

        // content delta
        if (delta.contains("content") && !delta["content"].is_null()) {
            out.content_delta = delta["content"].get<std::string>();
        }

        // reasoning_content delta（DeepSeek 等兼容格式）
        if (delta.contains("reasoning_content") && !delta["reasoning_content"].is_null()) {
            out.reasoning_delta = delta["reasoning_content"].get<std::string>();
        }

        // finish_reason
        if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
            auto finish_reason = choice["finish_reason"].get<std::string>();
            if (finish_reason == "stop" || finish_reason == "length") {
                out.is_final = true;
                // usage 信息
                if (json_obj.contains("usage") && !json_obj["usage"].is_null()) {
                    const auto& usage = json_obj["usage"];
                    out.prompt_tokens = usage.value("prompt_tokens", 0);
                    out.generated_tokens = usage.value("completion_tokens", 0);
                }
            }
        }

        return !out.content_delta.empty() || !out.reasoning_delta.empty() || out.is_final;

    } catch (const nlohmann::json::parse_error&) {
        return false;
    } catch (const std::exception&) {
        return false;
    }
#else
    // 无 JSON 库时的回退：直接作为文本
    out.content_delta = data;
    return true;
#endif
}

} // namespace agent
