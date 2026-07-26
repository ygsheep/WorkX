/**
 * @file react_loop.cpp
 * @brief ReActLoop 实现
 * @details Thought / Action / Observation 三阶段循环逻辑
 * @version 1.0.0
 * @date 2026-07
 */

#include "agent/core/react_loop.h"
#include "agent/api/i_stream_reader.h"

#include <cctype>
#include <filesystem>
#include <format>
#include <thread>

namespace agent {

// ============================================================
// 构造
// ============================================================

ReActLoop::ReActLoop(ICompletionProvider* provider,
                     std::shared_ptr<tool::ToolRegistry> registry,
                     Config config)
    : m_provider(provider)
    , m_registry(std::move(registry))
    , m_config(config)
{
    if (m_registry) {
        m_executor = std::make_unique<tool::ToolExecutor>(m_registry);
    }
}

// ============================================================
// build_request — 构建 CompletionRequest
// ============================================================

CompletionRequest ReActLoop::build_request(
    const std::vector<ChatMessage>& messages,
    const std::string& system_prompt,
    const nlohmann::json& tools_schema
) const {
    CompletionRequest request;
    request.stream = true;

    if (!system_prompt.empty()) {
        request.messages.push_back(ChatMessage::system(system_prompt));
    }

    for (const auto& msg : messages) {
        request.messages.push_back(msg);
    }

    // 注入工具 schema（启用 function calling）
    if (!tools_schema.is_null() && tools_schema.is_array() && !tools_schema.empty()) {
        request.tools = tools_schema;
    }

    return request;
}

// ============================================================
// execute_thought — 流式读取 LLM 响应
// ============================================================

ReActLoop::ThoughtResult ReActLoop::execute_thought(
    const CompletionRequest& request,
    const std::atomic<bool>& should_cancel,
    TokenCallback on_token
) {
    ThoughtResult result;

    // 提交推理请求
    auto reader = m_provider->submit_completion(request);
    if (!reader) {
        result.status = ThoughtResult::Error;
        return result;
    }

    // 流式累积 tool_use 的 input JSON
    struct PendingToolUse {
        std::string id;
        std::string name;
        std::string input_json;
    };
    std::vector<PendingToolUse> pending_tools;

    StreamChunk chunk;

    while (true) {
        if (should_cancel) {
            reader->cancel();
            result.status = ThoughtResult::Cancelled;
            break;
        }

        auto state = reader->next(
            [&should_cancel]() { return should_cancel.load(); },
            chunk
        );

        if (state == StreamState::HasData) {
            // 文本/推理增量
            if (!chunk.content_delta.empty() || !chunk.reasoning_delta.empty()) {
                result.content += chunk.content_delta;
                result.reasoning += chunk.reasoning_delta;

                if (on_token) {
                    on_token(chunk.content_delta, chunk.reasoning_delta);
                }
            }

            // tool_use content_block 开始
            if (chunk.is_tool_use_start) {
                pending_tools.push_back({
                    chunk.tool_use_id,
                    chunk.tool_name,
                    ""
                });
            }

            // tool_use input JSON 增量（流式拼接）
            if (chunk.is_tool_use_delta && !pending_tools.empty()) {
                pending_tools.back().input_json += chunk.tool_input_delta;
            } else if (chunk.is_tool_use_delta) {
                // 容错：模型未发送 tool_use_start 直接发送 arguments
                // （部分兼容模型如 Gemma 可能省略 id/name 首帧）
                pending_tools.push_back({"", "", chunk.tool_input_delta});
            }
        } else if (state == StreamState::Complete) {
            // 最后一个 chunk 可能携带 token 统计
            if (!chunk.content_delta.empty() || !chunk.reasoning_delta.empty()) {
                result.content += chunk.content_delta;
                result.reasoning += chunk.reasoning_delta;

                if (on_token) {
                    on_token(chunk.content_delta, chunk.reasoning_delta);
                }
            }
            result.prompt_tokens = chunk.prompt_tokens;
            result.generated_tokens = chunk.generated_tokens;
            result.prompt_ms = chunk.prompt_ms;
            result.generation_ms = chunk.generation_ms;
            result.status = ThoughtResult::Completed;
            break;
        } else if (state == StreamState::Error) {
            result.status = ThoughtResult::Error;
            break;
        } else if (state == StreamState::Cancelled) {
            result.status = ThoughtResult::Cancelled;
            break;
        }
    }

    // 将 pending_tools 转换为 ToolUse
    for (const auto& ptu : pending_tools) {
        ToolUse tu;
        tu.id = ptu.id;
        tu.name = ptu.name;
        try {
            tu.input = ptu.input_json.empty()
                ? nlohmann::json::object()
                : nlohmann::json::parse(ptu.input_json);
        } catch (const nlohmann::json::parse_error&) {
            tu.input = nlohmann::json::object();
        }
        result.tool_uses.push_back(std::move(tu));
    }

    // Fallback：若模型未走标准 tool_calls 协议（部分本地推理后端如 lm-studio /
    // llama.cpp 在 auto 模式下会用文本内嵌 JSON 描述工具调用），从 content 中
    // 扫描形如 {"name":"Write","arguments":{...}} 或 {"tool":"Write","input":{...}}
    // 的 JSON 块解析为 ToolUse
    if (result.tool_uses.empty() && !result.content.empty()) {
        parse_embedded_tool_calls(result.content, result.tool_uses);
    }

    return result;
}

// ============================================================
// parse_embedded_tool_calls — 文本内嵌工具调用 fallback 解析
// ============================================================

void ReActLoop::parse_embedded_tool_calls(const std::string& content,
                                          std::vector<ToolUse>& out_tools) {
    out_tools.clear();

    // 扫描 content 中的每个 '{' 尝试解析为 JSON 对象
    // 识别 { "name": "Write", "arguments": {...} } 或 { "tool": "Write", "input": {...} }
    // 兼容 GLM / Qwen / Llama 等 GGUF 模型常见内嵌格式
    for (size_t i = 0; i < content.size(); ++i) {
        if (content[i] != '{') continue;

        // 尝试从此处解析 JSON，需要找到匹配的 '}'
        int depth = 0;
        size_t j = i;
        bool in_string = false;
        bool escape = false;
        for (; j < content.size(); ++j) {
            char c = content[j];
            if (escape) { escape = false; continue; }
            if (c == '\\') { escape = true; continue; }
            if (c == '"') { in_string = !in_string; continue; }
            if (in_string) continue;
            if (c == '{') depth++;
            else if (c == '}') {
                depth--;
                if (depth == 0) break;  // 闭合
            }
        }
        if (depth != 0) continue;  // 未配对，跳过

        std::string json_str = content.substr(i, j - i + 1);
        nlohmann::json obj;
        try {
            obj = nlohmann::json::parse(json_str);
        } catch (...) {
            continue;  // 不是合法 JSON，跳过
        }
        if (!obj.is_object()) continue;

        // 识别工具名字段：name / tool / function
        std::string tool_name = obj.value("name", "");
        if (tool_name.empty()) tool_name = obj.value("tool", "");
        if (tool_name.empty()) {
            // OpenAI 风格：{"type":"function","function":{"name":"...","arguments":"..."}}
            if (obj.contains("function") && obj["function"].is_object()) {
                tool_name = obj["function"].value("name", "");
            }
        }
        if (tool_name.empty()) {
            // 可能有 reasoning text 中的 JSON，但不是 tool 调用，跳过
            continue;
        }

        // 识别参数字段：arguments / input / parameters
        nlohmann::json tool_input;
        if (obj.contains("arguments")) {
            const auto& args = obj["arguments"];
            if (args.is_string()) {
                // arguments 可能是 JSON 字符串
                try { tool_input = nlohmann::json::parse(args.get<std::string>()); }
                catch (...) { tool_input = nlohmann::json::object(); }
            } else if (args.is_object()) {
                tool_input = args;
            }
        } else if (obj.contains("input") && obj["input"].is_object()) {
            tool_input = obj["input"];
        } else if (obj.contains("parameters") && obj["parameters"].is_object()) {
            tool_input = obj["parameters"];
        } else if (obj.contains("function") && obj["function"].is_object()) {
            const auto& func = obj["function"];
            if (func.contains("arguments")) {
                const auto& args = func["arguments"];
                if (args.is_string()) {
                    try { tool_input = nlohmann::json::parse(args.get<std::string>()); }
                    catch (...) { tool_input = nlohmann::json::object(); }
                } else if (args.is_object()) {
                    tool_input = args;
                }
            }
        } else {
            tool_input = nlohmann::json::object();
        }

        if (!tool_input.is_object()) tool_input = nlohmann::json::object();

        // 排除明显不是工具调用的误识别（如 tool_name 为 "type" 等保留字）
        // 要求工具名至少 2 个字符且首字符为字母
        if (tool_name.size() < 2 || !std::isalpha(static_cast<unsigned char>(tool_name[0]))) {
            continue;
        }

        ToolUse tu;
        tu.id = "embedded_" + std::to_string(out_tools.size());
        tu.name = std::move(tool_name);
        tu.input = std::move(tool_input);
        out_tools.push_back(std::move(tu));
    }
}

// ============================================================
// run — ReAct 主循环
// ============================================================

ReActResult ReActLoop::run(
    std::vector<ChatMessage>& messages,
    const std::string& system_prompt,
    const nlohmann::json& tools_schema,
    const std::atomic<bool>& should_cancel,
    StepCallback on_step,
    TokenCallback on_token
) {
    ReActResult result;
    auto loop_start = std::chrono::steady_clock::now();
    int step_counter = 0;

    for (int iteration = 1; iteration <= m_config.max_iterations; ++iteration) {
        result.total_iterations = iteration;

        // ================================================================
        // === Thought 阶段 ===
        // ================================================================

        auto thought_start = std::chrono::steady_clock::now();

        CompletionRequest request = build_request(messages, system_prompt, tools_schema);
        ThoughtResult thought = execute_thought(request, should_cancel, on_token);

        auto thought_end = std::chrono::steady_clock::now();
        double thought_ms = std::chrono::duration<double, std::milli>(
            thought_end - thought_start).count();

        // --- 处理 Thought 异常状态 ---

        if (thought.status == ThoughtResult::Cancelled) {
            result.was_interrupted = true;
            result.partial_content = thought.content;
            result.partial_reasoning = thought.reasoning;
            break;
        }

        if (thought.status == ThoughtResult::Error) {
            result.was_error = true;
            result.error_message = "Stream error during Thought phase";
            result.partial_content = thought.content;
            result.partial_reasoning = thought.reasoning;
            break;
        }

        // --- 记录 Thought 步骤 ---
        {
            ReActStep step;
            step.type = ReActStepType::Thought;
            step.step_number = ++step_counter;
            step.thought_text = thought.content;
            step.reasoning = thought.reasoning;
            step.tool_uses = thought.tool_uses;
            step.duration_ms = thought_ms;
            result.steps.push_back(step);

            if (on_step) {
                on_step(step);
            }
        }

        // 保存 token 统计（每次 Thought 更新，最终保留最后一次）
        result.prompt_tokens = thought.prompt_tokens;
        result.generated_tokens = thought.generated_tokens;
        result.prompt_ms = thought.prompt_ms;
        result.generation_ms = thought.generation_ms;

        // ================================================================
        // === 终止判断：无 tool_use → FinalAnswer ===
        // ================================================================

        if (thought.tool_uses.empty()) {
            // LLM 给出最终回复，无需工具调用
            messages.push_back(ChatMessage::assistant(thought.content));
            if (!thought.reasoning.empty()) {
                messages.back().reasoning_content = thought.reasoning;
            }

            result.final_answer = thought.content;
            result.final_reasoning = thought.reasoning;

            // 记录 FinalAnswer 步骤
            {
                ReActStep step;
                step.type = ReActStepType::FinalAnswer;
                step.step_number = ++step_counter;
                step.thought_text = thought.content;
                step.reasoning = thought.reasoning;
                step.duration_ms = thought_ms;
                result.steps.push_back(step);

                if (on_step) {
                    on_step(step);
                }
            }

            break;  // 正常退出循环
        }

        // ================================================================
        // === 有 tool_use：构建 assistant 消息 ===
        // ================================================================

        ChatMessage assistant_msg = ChatMessage::assistant(thought.content);
        if (!thought.reasoning.empty()) {
            assistant_msg.reasoning_content = thought.reasoning;
        }
        for (auto& tu : thought.tool_uses) {
            assistant_msg.tool_uses.push_back(tu);
        }
        messages.push_back(std::move(assistant_msg));

        // ================================================================
        // === Action + Observation 阶段 ===
        // ================================================================

        // 无 executor 时：以错误作为 tool_result 回传
        if (!m_executor) {
            for (const auto& tu : thought.tool_uses) {
                // Action 步骤
                ReActStep action_step;
                action_step.type = ReActStepType::Action;
                action_step.step_number = ++step_counter;
                action_step.tool_name = tu.name;
                action_step.tool_input = tu.input;
                result.steps.push_back(action_step);
                if (on_step) on_step(action_step);

                // Observation 步骤
                std::string err_msg = "Error: tool executor not configured";
                messages.push_back(ChatMessage::tool_result(tu.id, tu.name, err_msg));

                ReActStep obs_step;
                obs_step.type = ReActStepType::Observation;
                obs_step.step_number = ++step_counter;
                obs_step.observation = err_msg;
                obs_step.is_error = true;
                result.steps.push_back(obs_step);
                if (on_step) on_step(obs_step);

                result.total_tool_calls++;
            }
            continue;  // 继续下一轮 Thought
        }

        // 有 executor：执行每个 tool_use
        tool::ToolContext ctx;
        ctx.cwd = std::filesystem::current_path().string();
        ctx.session_id = "default";

        for (const auto& tu : thought.tool_uses) {
            // --- Action 阶段 ---

            auto action_start = std::chrono::steady_clock::now();

            // 记录 Action 步骤（on_step 回调发布 ToolCallEvent，UI 显示工具图标）
            {
                ReActStep step;
                step.type = ReActStepType::Action;
                step.step_number = ++step_counter;
                step.tool_name = tu.name;
                step.tool_input = tu.input;
                result.steps.push_back(step);

                if (on_step) {
                    on_step(step);
                }
            }

            // 执行工具（异常安全：工具抛出异常时转为错误 Observation，不让循环崩溃）
            std::string result_text;
            bool tool_error = false;
            try {
                auto exec_result = m_executor->execute(tu.name, tu.input, ctx);
                result_text = exec_result.result.to_string();
                tool_error = exec_result.is_error;
            } catch (const std::exception& e) {
                result_text = std::format("Error: tool '{}' threw exception: {}", tu.name, e.what());
                tool_error = true;
            } catch (...) {
                result_text = std::format("Error: tool '{}' threw unknown exception", tu.name);
                tool_error = true;
            }

            auto action_end = std::chrono::steady_clock::now();
            double action_ms = std::chrono::duration<double, std::milli>(
                action_end - action_start).count();

            // --- Observation 阶段 ---

            // 添加 tool_result 消息到对话历史
            messages.push_back(ChatMessage::tool_result(tu.id, tu.name, result_text));

            // 记录 Observation 步骤
            {
                ReActStep step;
                step.type = ReActStepType::Observation;
                step.step_number = ++step_counter;
                step.observation = result_text;
                step.is_error = tool_error;
                step.duration_ms = action_ms;
                result.steps.push_back(step);

                if (on_step) {
                    on_step(step);
                }
            }

            result.total_tool_calls++;
        }

        // 继续下一轮 Thought（LLM 根据 tool_result 决定下一步）
    }

    // ================================================================
    // 循环结束处理
    // ================================================================

    auto loop_end = std::chrono::steady_clock::now();
    result.total_duration_ms = std::chrono::duration<double, std::milli>(
        loop_end - loop_start).count();

    // 超过最大迭代数
    if (!result.was_interrupted && !result.was_error && result.final_answer.empty()) {
        result.was_error = true;
        result.error_message = std::format("Agent loop reached max iterations ({})",
                                           m_config.max_iterations);
    }

    return result;
}

} // namespace agent
