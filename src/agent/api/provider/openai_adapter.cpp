/**
 * @file openai_adapter.cpp
 * @brief OpenAI 协议适配器实现
 * @details 从原有的 RemoteBackend 提取，封装为独立适配器
 * @version 1.0.0
 * @date 2026-07
 */

#include "agent/api/provider/openai_adapter.h"

#include <nlohmann/json.hpp>

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
    nlohmann::json j;
    j["model"] = model_name;
    j["stream"] = request.stream;

    // 流式传输时启用 usage 上报（OpenAI 要求 stream_options.include_usage=true）
    // 否则流式响应中 usage 永远为 0
    if (request.stream) {
        j["stream_options"] = {{"include_usage", true}};
    }

    auto& messages = j["messages"];
    for (const auto& msg : request.messages) {
        nlohmann::json m;
        switch (msg.role) {
            case ChatMessage::Role::System:    m["role"] = "system"; break;
            case ChatMessage::Role::User:      m["role"] = "user"; break;
            case ChatMessage::Role::Assistant: m["role"] = "assistant"; break;
            case ChatMessage::Role::Tool:      m["role"] = "tool"; break;
        }
        // content：assistant 带 tool_calls 时 content 可能为空，用 null 而非空字符串
        // Tool 角色 + is_error=true：用 <tool_use_error> 标签包裹，对齐 Claude Code 语义，
        // 让模型明确感知工具失败（OpenAI 格式无 is_error 字段，只能通过文本标识）
        if (msg.role == ChatMessage::Role::Assistant && !msg.tool_uses.empty() && msg.content.empty()) {
            m["content"] = nullptr;
        } else if (msg.role == ChatMessage::Role::Tool && msg.is_error) {
            m["content"] = "<tool_use_error>" + msg.content + "</tool_use_error>";
        } else {
            m["content"] = msg.content;
        }
        // DS_CACHE P2：reasoning_content 可选往返
        // 默认不发送（非标准字段，会干扰 Gemma 等模型）
        // 开启时（DeepSeek-reasoner 等 thinking 模型）：把 CoT 作为前缀发送，提升多轮缓存命中
        if (m_send_reasoning_content && msg.role == ChatMessage::Role::Assistant
            && !msg.reasoning_content.empty()) {
            m["reasoning_content"] = msg.reasoning_content;
        }
        if (msg.role == ChatMessage::Role::Tool) {
            if (msg.tool_call_id.empty()) {
                continue;  // 跳过无效 Tool 消息
            }
            m["tool_call_id"] = msg.tool_call_id;
        }
        // assistant 工具调用（OpenAI tool_calls 格式）
        if (msg.role == ChatMessage::Role::Assistant && !msg.tool_uses.empty()) {
            nlohmann::json tool_calls = nlohmann::json::array();
            for (const auto& tu : msg.tool_uses) {
                tool_calls.push_back({
                    {"id", tu.id},
                    {"type", "function"},
                    {"function", {
                        {"name", tu.name},
                        {"arguments", tu.input.is_null() ? "{}" : tu.input.dump()}
                    }}
                });
            }
            m["tool_calls"] = std::move(tool_calls);
        }
        messages.push_back(m);
    }

    // tools 转换：内部格式 {name, description, input_schema} → OpenAI {type:"function", function:{...}}
    if (request.has_tools()) {
        nlohmann::json tools_arr = nlohmann::json::array();
        for (const auto& tool : request.tools) {
            tools_arr.push_back({
                {"type", "function"},
                {"function", {
                    {"name", tool.value("name", "")},
                    {"description", tool.value("description", "")},
                    {"parameters", tool.contains("input_schema") && !tool["input_schema"].is_null()
                        ? tool["input_schema"] : nlohmann::json::object()}
                }}
            });
        }
        j["tools"] = std::move(tools_arr);
        // 显式声明 tool_choice=auto：部分本地推理后端（lm-studio / llama.cpp）
        // 在未显式声明时会倾向于用文字描述工具调用而非走标准 function calling 协议
        j["tool_choice"] = "auto";
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
}

bool OpenAIAdapter::parse_sse_event(const std::string& /*event_type*/,
                                     const std::string& data,
                                     StreamChunk& out) const {
    // OpenAI [DONE] 标记
    if (data == "[DONE]") {
        out.is_final = true;
        return true;
    }

    try {
        auto json_obj = nlohmann::json::parse(data);

        // 错误事件：把错误信息塞入 content_delta 让用户看到，并标记 final 终止流
        // （StreamChunk 暂无 error_message 字段，新增字段留待后续重构）
        if (json_obj.contains("error")) {
            const auto& err = json_obj.value("error", nlohmann::json::object());
            std::string err_msg = err.value("message", "Unknown OpenAI error");
            out.content_delta = std::string("[OpenAI Error] ") + err_msg;
            out.is_final = true;
            return true;
        }

        // choices 空：可能是 usage chunk（开启 include_usage=true 后最后 chunk 的 choices 为空）
        if (!json_obj.contains("choices") || json_obj["choices"].empty()) {
            if (json_obj.contains("usage") && !json_obj["usage"].is_null()) {
                const auto& usage = json_obj.value("usage", nlohmann::json::object());
                out.prompt_tokens = usage.value("prompt_tokens", 0);
                out.generated_tokens = usage.value("completion_tokens", 0);
                // DeepSeek 硬盘缓存命中字段（其它 provider 无此字段时为 0）
                out.prompt_cache_hit_tokens = usage.value("prompt_cache_hit_tokens", 0);
                out.prompt_cache_miss_tokens = usage.value("prompt_cache_miss_tokens", 0);
                return true;  // 仅更新 usage，不算 final
            }
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

        // tool_calls 增量（function calling）
        // OpenAI 实际行为：多个 tool_call 时按 index 顺序分开发送，不会在同个 chunk 内混合
        // 遍历数组处理第一个非空元素，StreamChunk 只能存一个 tool_call 增量
        if (delta.contains("tool_calls") && !delta["tool_calls"].empty()) {
            for (const auto& tc : delta["tool_calls"]) {
                const auto& func = tc.value("function", nlohmann::json::object());

                // 首次出现：带 id 和/或 function.name
                // （标准 OpenAI 格式带 id；部分兼容模型如 Gemma 可能只有 function.name）
                const bool has_id = tc.contains("id") && !tc["id"].is_null();
                const bool has_name = func.contains("name") && !func["name"].is_null()
                                      && !func["name"].get<std::string>().empty();
                if (has_id || has_name) {
                    out.is_tool_use_start = true;
                    if (has_id) {
                        out.tool_use_id = tc["id"].get<std::string>();
                    }
                    out.tool_name = func.value("name", "");
                    // 首次可能也带 arguments 增量
                    if (func.contains("arguments") && !func["arguments"].is_null()) {
                        out.is_tool_use_delta = true;
                        out.tool_input_delta = func["arguments"].get<std::string>();
                    }
                    return true;  // 一次只处理一个 tool_call start
                }

                // 后续：只有 arguments 增量
                if (func.contains("arguments") && !func["arguments"].is_null()) {
                    out.is_tool_use_delta = true;
                    out.tool_input_delta = func["arguments"].get<std::string>();
                    return true;  // 一次只处理一个 delta
                }
            }
        }

        // finish_reason：新增 content_filter（OpenAI 内容审核触发时返回）
        if (choice.contains("finish_reason") && !choice["finish_reason"].is_null()) {
            auto finish_reason = choice["finish_reason"].get<std::string>();
            if (finish_reason == "stop" || finish_reason == "length" ||
                finish_reason == "tool_calls" || finish_reason == "content_filter") {
                out.is_final = true;
                // content_filter 时附加提示信息
                if (finish_reason == "content_filter") {
                    out.content_delta = "[Content filtered by provider]";
                }
                // usage 信息
                if (json_obj.contains("usage") && !json_obj["usage"].is_null()) {
                    const auto& usage = json_obj.value("usage", nlohmann::json::object());
                    out.prompt_tokens = usage.value("prompt_tokens", 0);
                    out.generated_tokens = usage.value("completion_tokens", 0);
                    // DeepSeek 硬盘缓存命中字段（其它 provider 无此字段时为 0）
                    out.prompt_cache_hit_tokens = usage.value("prompt_cache_hit_tokens", 0);
                    out.prompt_cache_miss_tokens = usage.value("prompt_cache_miss_tokens", 0);
                }
            }
        }

        return !out.content_delta.empty() || !out.reasoning_delta.empty() ||
               out.is_tool_use_start || out.is_tool_use_delta || out.is_final;

    } catch (const nlohmann::json::parse_error&) {
        return false;
    } catch (const std::exception&) {
        return false;
    }
}

IProviderAdapter::ModelEndpointResult OpenAIAdapter::get_models_endpoint() const {
    return {true, "/v1/models"};
}

} // namespace agent
