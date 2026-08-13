/**
 * @file react_loop.cpp
 * @brief ReActLoop 实现
 * @details Thought / Action / Observation 三阶段循环逻辑
 * @version 1.0.0
 * @date 2026-07
 */

#include "agent/core/react_loop.h"
#include "agent/core/react_observer.h"
#include "agent/api/i_stream_reader.h"
#include "agent/compact/prefix_shape.h"  // DS_CACHE M-2: normalize_tools_schema
#include "agent/skill/inclaude/conditional.h"
#include "liblogger/logger.h"

#include <cctype>
#include <cassert>
#include <stdexcept>
#include <filesystem>
#include <format>
#include <future>
#include <thread>
#include <utility>

namespace agent {

// ============================================================
// 构造
// ============================================================

ReActLoop::ReActLoop(ICompletionProvider* provider,
                     std::shared_ptr<tool::ToolRegistry> registry,
                     Config config,
                     IConfigManager* config_manager,
                     ITaskManager* task_manager,
                     std::string cwd,
                     CacheAwareCompactor* external_compactor,
                     IEventBus* event_bus,
                     skill::TouchCollector* touch_collector,
                     std::function<void()> file_index_invalidator)
    : m_provider(provider)
    , m_registry(std::move(registry))
    , m_config(config)
    , m_owned_compactor(external_compactor ? nullptr
                                           : std::make_unique<CacheAwareCompactor>(m_config.compactor_cfg))
    , m_compactor(external_compactor ? *external_compactor : *m_owned_compactor)
    , m_config_manager(config_manager)
    , m_task_manager(task_manager)
    , m_event_bus(event_bus)
    , m_cwd(std::move(cwd))
    , m_touch_collector(touch_collector)
    , m_file_index_invalidator(std::move(file_index_invalidator))
{
    // issue #15-F: 构造函数不变量从 assert 改为 throw，避免 Debug 构建直接 abort
    // 构造失败抛 std::invalid_argument 是 C++ 标准模式，调用方可用 try/catch 处理
    if (provider == nullptr) {
        throw std::invalid_argument("ReActLoop: provider must not be null");
    }
    if (m_config_manager == nullptr) {
        throw std::invalid_argument(
            "ReActLoop: config_manager must not be null (H-5: explicit DI required)");
    }
    // cwd 为空时回退到进程当前目录（会话启动时捕获更稳定，避免运行中 cwd 漂移）
    if (m_cwd.empty()) {
        m_cwd = std::filesystem::current_path().string();
    }
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
    // DS_CACHE M-2：按 function.name 排序后再赋值，消除注册顺序抖动导致的缓存击穿。
    // 与 prefix_shape::normalize_tools_schema 复用同一逻辑，保证发送字节与 hash 计算一致。
    if (!tools_schema.is_null() && tools_schema.is_array() && !tools_schema.empty()) {
        request.tools = normalize_tools_schema(tools_schema);
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
            result.cache_creation_input_tokens = chunk.cache_creation_input_tokens;
            result.cache_read_input_tokens = chunk.cache_read_input_tokens;
            result.prompt_cache_hit_tokens = chunk.prompt_cache_hit_tokens;
            result.prompt_cache_miss_tokens = chunk.prompt_cache_miss_tokens;
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
    IReActObserver* observer
) {
    // 3.2：将 IReActObserver 适配为 StepCallback + TokenCallback
    StepCallback on_step = nullptr;
    TokenCallback on_token = nullptr;

    if (observer) {
        on_step = [observer](const ReActStep& step) {
            switch (step.type) {
                case ReActStepType::Thought:
                    observer->on_thought(step);
                    break;
                case ReActStepType::Action:
                    observer->on_action(step);
                    break;
                case ReActStepType::Observation:
                    observer->on_observation(step);
                    break;
                case ReActStepType::FinalAnswer:
                    observer->on_final_answer(step);
                    break;
            }
        };
        on_token = [observer](const std::string& content_delta,
                              const std::string& reasoning_delta) {
            observer->on_token(content_delta, reasoning_delta);
        };
    }

    return run(messages, system_prompt, tools_schema, should_cancel,
               std::move(on_step), std::move(on_token));
}

// ============================================================
// run — ReAct 主循环（回调版本）
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

        LOG_INFO("[react_loop] iteration={} thought_begin, messages={}, has_tools={}",
                 iteration, messages.size(),
                 (!tools_schema.is_null() && tools_schema.is_array() && !tools_schema.empty()));

        // DS_CACHE M-6：压缩点移至 turn 间（仅 iteration == 1 时执行）
        // 原实现每 iteration > 1 都调用，会在单 turn 多步推理中过早截短前序 tool_result。
        // Plan 意图是 turn 间压缩：每 turn 开始时压缩一次，避免 turn 内工具链被破坏。
        // compactor 状态由 ChatSession 持有跨 turn 持久化（H-3）。
        if (iteration == 1) {
            auto compact_result = m_compactor.maybe_compact(messages);
            if (compact_result.action != CacheAwareCompactor::Action::None
                && compact_result.action != CacheAwareCompactor::Action::SoftNotice) {
                LOG_INFO("[react_loop] turn-start compact action={}, snipped={}, folded={}, "
                         "tokens {} -> {}, rewrite_version={}",
                         static_cast<int>(compact_result.action),
                         compact_result.snipped_count, compact_result.compacted_count,
                         compact_result.tokens_before, compact_result.tokens_after,
                         m_compactor.rewrite_version());
            }
        }

        CompletionRequest request = build_request(messages, system_prompt, tools_schema);
        ThoughtResult thought = execute_thought(request, should_cancel, on_token);

        auto thought_end = std::chrono::steady_clock::now();
        double thought_ms = std::chrono::duration<double, std::milli>(
            thought_end - thought_start).count();

        LOG_INFO("[react_loop] iteration={} thought_end, status={}, content_len={}, "
                 "reasoning_len={}, tool_uses={}, prompt_tokens={}, generated_tokens={}, "
                 "cache_creation={}, cache_read={}, prompt_ms={:.1f}, generation_ms={:.1f}, "
                 "thought_ms={:.1f}",
                 iteration, static_cast<int>(thought.status),
                 thought.content.size(), thought.reasoning.size(),
                 thought.tool_uses.size(),
                 thought.prompt_tokens, thought.generated_tokens,
                 thought.cache_creation_input_tokens, thought.cache_read_input_tokens,
                 thought.prompt_ms, thought.generation_ms, thought_ms);

        // --- 处理 Thought 异常状态 ---

        if (thought.status == ThoughtResult::Cancelled) {
            LOG_WARN("[react_loop] iteration={} thought cancelled by user", iteration);
            result.was_interrupted = true;
            result.partial_content = thought.content;
            result.partial_reasoning = thought.reasoning;
            break;
        }

        if (thought.status == ThoughtResult::Error) {
            LOG_ERROR("[react_loop] iteration={} thought stream error", iteration);
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
        result.cache_creation_input_tokens = thought.cache_creation_input_tokens;
        result.cache_read_input_tokens = thought.cache_read_input_tokens;
        result.prompt_cache_hit_tokens = thought.prompt_cache_hit_tokens;
        result.prompt_cache_miss_tokens = thought.prompt_cache_miss_tokens;
        result.prompt_ms = thought.prompt_ms;
        result.generation_ms = thought.generation_ms;

        // ================================================================
        // === 终止判断：无 tool_use → FinalAnswer ===
        // ================================================================

        if (thought.tool_uses.empty()) {
            // LLM 给出最终回复，无需工具调用
            LOG_INFO("[react_loop] iteration={} final_answer, content_len={}",
                     iteration, thought.content.size());
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
                messages.push_back(ChatMessage::tool_result(tu.id, tu.name, err_msg, true));

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

        // 有 executor：3.1 并行执行所有 tool_use
        // Phase 3 已审计：所有工具 call() const，无实例可变状态，可安全并行
        LOG_INFO("[react_loop] iteration={} action_begin, parallel_tools={}",
                 iteration, thought.tool_uses.size());

        tool::ToolContext ctx;
        ctx.cwd = m_cwd;  // 使用会话启动时捕获的 cwd，避免运行中 cwd 漂移
        ctx.session_id = "default";
        // 2.3 修复：将外部取消信号绑定到 ToolContext，工具可即时感知中断
        ctx.cancel_flag = &should_cancel;
        // H-5：注入配置管理器（非空），工具通过 ctx.config_manager() 访问
        ctx.config_manager_ptr = m_config_manager;
        // BashTool 后台任务 DI：注入任务管理器（可选）
        ctx.task_manager_ptr = m_task_manager;
        // AskUserTool 事件发布 DI：注入事件总线（可选）
        ctx.event_bus_ptr = m_event_bus;
        // conditional skills：注入 touch 回调（可选），工具上报访问过的文件
        if (m_touch_collector) {
            ctx.touch_callback = [collector = m_touch_collector](const std::string& path) {
                collector->add(path);
            };
        }
        // 宿主文件索引失效回调（可选）：FileWriteTool 写文件后通知宿主重建索引
        ctx.on_file_system_changed = m_file_index_invalidator;
        // #28：注入会话级权限模式 + 模式变更回调（EnterPlanMode/ExitPlanMode 工具切换）
        ctx.permission_mode = m_permission_mode;
        ctx.on_permission_mode_changed = [this](tool::PermissionMode mode) {
            m_permission_mode = mode;
        };
        // #26：注入推理提供者 + 工具注册表（AgentTool 启动子 Agent 用）
        ctx.provider_ptr = m_provider;
        ctx.tool_registry = m_registry;
        // #28 评审 #1/#3：进入计划模式——保存原模式、切换 Plan；Bypass 禁止降级、已在 Plan 则幂等拒绝
        ctx.on_enter_plan_mode = [this]() -> bool {
            if (m_permission_mode == tool::PermissionMode::BypassPermissions) {
                return false;  // Bypass 全权模式禁止降级到 Plan
            }
            if (m_in_plan_mode) {
                return false;  // 已在 Plan，幂等
            }
            m_permission_mode_before_plan = m_permission_mode;
            m_permission_mode = tool::PermissionMode::Plan;
            m_in_plan_mode = true;
            return true;
        };
        // #28 评审 #1：退出计划模式——恢复进入前的原模式，而非硬编码 Default
        ctx.on_exit_plan_mode = [this]() {
            if (m_in_plan_mode) {
                m_permission_mode = m_permission_mode_before_plan;
                m_in_plan_mode = false;
            }
        };

        // 1. 同步发布所有 Action 步骤（UI 即时反馈工具调用开始）
        for (const auto& tu : thought.tool_uses) {
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

        // 2. 异步并行执行所有 tool_use
        struct ToolExecution {
            std::string tool_use_id;
            std::string tool_name;
            std::future<std::pair<std::string, bool>> future;  // {result_text, is_error}
        };

        std::vector<ToolExecution> executions;
        executions.reserve(thought.tool_uses.size());

        for (const auto& tu : thought.tool_uses) {
            LOG_DEBUG("[react_loop] launching async tool_use, id={}, name={}",
                      tu.id, tu.name);
            auto future = std::async(std::launch::async,
                [this, &ctx, &tu]() -> std::pair<std::string, bool> {
                    try {
                        auto exec_result = m_executor->execute(tu.name, tu.input, ctx);
                        if (exec_result.is_err()) {
                            // V2-4：错误由 ResultV2 承载，message 用于 LLM 可读反馈
                            return {std::format("Error: {}", exec_result.error().message), true};
                        }
                        return {exec_result.value().result.to_string(), false};
                    } catch (const std::exception& e) {
                        return {std::format("Error: tool '{}' threw exception: {}",
                                            tu.name, e.what()), true};
                    } catch (...) {
                        return {std::format("Error: tool '{}' threw unknown exception",
                                            tu.name), true};
                    }
                });

            executions.push_back({tu.id, tu.name, std::move(future)});
            result.total_tool_calls++;
        }

        // 3. 等待所有工具完成，按原始顺序生成 Observation
        for (auto& exec : executions) {
            auto action_start = std::chrono::steady_clock::now();

            // 协作式等待（#23 P1）：工具已绑定 ctx.cancel_flag=should_cancel，
            // 用户中断后工具会自己退出（bash 由 subprocess 杀进程树），
            // 这里用 wait_for 轮询替代阻塞的 future.get()，使打断即时生效，
            // 且 loop 在工具返回前不丢消息（Observation 照常先生成）。
            // 注意：不做超时脱离 —— std::async(future) 析构会隐式等待线程，
            //           孤儿线程不可真正 detach（残余风险记入 P3 保险丝）。
            while (exec.future.wait_for(std::chrono::milliseconds(100))
                   != std::future_status::ready) {
                if (should_cancel) {
                    LOG_DEBUG("[react_loop] tool={} waiting for cooperative "
                              "cancel (user interrupt)", exec.tool_name);
                }
            }

            auto [result_text, tool_error] = exec.future.get();

            auto action_end = std::chrono::steady_clock::now();
            double action_ms = std::chrono::duration<double, std::milli>(
                action_end - action_start).count();

            // --- Observation 阶段 ---

            // 添加 tool_result 消息到对话历史
            messages.push_back(ChatMessage::tool_result(
                exec.tool_use_id, exec.tool_name, result_text, tool_error));

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

            LOG_INFO("[react_loop] tool={} completed, is_error={}, duration={}ms",
                     exec.tool_name, tool_error, action_ms);
        }

        // 继续下一轮 Thought（LLM 根据 tool_result 决定下一步）
    }

    // ================================================================
    // 循环结束处理
    // ================================================================

    auto loop_end = std::chrono::steady_clock::now();
    result.total_duration_ms = std::chrono::duration<double, std::milli>(
        loop_end - loop_start).count();

    // DS_CACHE H-2：回填压缩器改写版本号，供 ChatSession 传入 capture_shape
    // 使 compare_shape 的 log_rewrite 归因在 compact 改写历史后能正确触发
    result.rewrite_version = m_compactor.rewrite_version();

    // 超过最大迭代数：仅当真正跑满 max_iterations 才报错
    // 注意：LLM 返回空 content + 无 tool_use 时也会 break 退出，此时 final_answer 为空，
    // 但属于正常退出（LLM 主动结束），不应误判为 max iterations
    if (!result.was_interrupted && !result.was_error
        && result.final_answer.empty()
        && result.total_iterations >= m_config.max_iterations) {
        result.was_error = true;
        result.error_message = std::format("Agent loop reached max iterations ({})",
                                           m_config.max_iterations);
        LOG_WARN("[react_loop] max iterations reached={}, total_duration_ms={:.1f}, "
                 "total_tool_calls={}",
                 m_config.max_iterations, result.total_duration_ms,
                 result.total_tool_calls);
    } else {
        LOG_INFO("[react_loop] loop end, iterations={}, total_duration_ms={:.1f}, "
                 "total_tool_calls={}, was_interrupted={}, was_error={}",
                 result.total_iterations, result.total_duration_ms,
                 result.total_tool_calls, result.was_interrupted, result.was_error);
    }

    return result;
}

} // namespace agent
