/**
 * @file agent_tool.cpp
 * @brief AgentTool 实现
 * @details 子 Agent 调度工具的具体实现
 * @version 1.3.1
 * @date 2026-07
 */

#include "agent/tool/AgentTool/agent_tool.h"

#include <random>
#include <format>
#include <algorithm>
#include <mutex>

#include "agent/api/chat_types.h"
#include "agent/core/react_loop.h"
#include "core/task/task_manager.h"
#include "core/utils/error.h"
#include "core/events/agent_events.h"

namespace agent::tool {

namespace {

/// @brief Agent 工具名（防递归排除 + 工具元信息共用，避免硬编码漂移）
constexpr const char* kAgentToolName = "Agent";

/// @brief 生成任务 id（对齐 TS generateTaskId：'a' 前缀 + 8 个随机小写字母数字）
std::string generate_task_id() {
    static constexpr char kAlphabet[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    // 复用随机源（MSVC 上 std::random_device 每次构造开销大，批量调度场景明显）
    // L-2：std::random_device 与 uniform_int_distribution 均非线程安全，加锁保护
    static std::mutex s_mutex;
    static std::random_device rd;
    std::lock_guard<std::mutex> lock(s_mutex);
    std::uniform_int_distribution<size_t> dist(0, sizeof(kAlphabet) - 2);
    std::string id = "a";
    id.reserve(9);
    for (int i = 0; i < 8; ++i) {
        id += kAlphabet[dist(rd)];
    }
    return id;
}

/// @brief 用 ", " 连接任务 id 列表（v1.2.0 批量调度返回消息）
std::string fmt_join_ids(const std::vector<std::string>& ids) {
    std::string out;
    for (size_t i = 0; i < ids.size(); ++i) {
        if (i) out += ", ";
        out += ids[i];
    }
    return out;
}

/// @brief 格式化任务数量描述（单复数："1 task" / "3 tasks"）
std::string fmt_task_count(size_t n) {
    return std::format("{} {}", n, n == 1 ? "task" : "tasks");
}

/// @brief 格式化 ReAct 步骤为输出行（写入 Task 输出缓冲）
std::string format_step_line(const ReActStep& step) {
    switch (step.type) {
        case ReActStepType::Thought:
            return step.thought_text.empty() ? std::string{}
                                             : std::format("[{}] Thought: {}", step.step_number, step.thought_text);
        case ReActStepType::Action:
            return std::format("[{}] Tool: {}", step.step_number, step.tool_name);
        case ReActStepType::Observation:
            return step.observation.empty() ? std::string{}
                                            : std::format("[{}] Observation: {}", step.step_number, step.observation);
        case ReActStepType::FinalAnswer:
            return step.thought_text.empty() ? std::string{}
                                            : std::format("[{}] Final: {}", step.step_number, step.thought_text);
    }
    return {};
}

/// @brief 步骤类型 → 字符串（v1.2.0 进度事件 step_type 字段）
const char* step_type_str(ReActStepType type) {
    switch (type) {
        case ReActStepType::Thought:     return "thought";
        case ReActStepType::Action:      return "action";
        case ReActStepType::Observation: return "observation";
        case ReActStepType::FinalAnswer: return "final";
    }
    return "unknown";
}

/// @brief 子 Agent 启动参数（聚合，避免长参数列表）
struct SubAgentLaunchOptions {
    std::string task_id;                         ///< 任务 id（AgentTool 生成的 'a'+8 随机）
    std::string prompt;                          ///< 子 Agent 任务 prompt
    std::vector<std::string> tool_whitelist;     ///< 工具白名单（空 → 全部已注册工具）
    ICompletionProvider* provider = nullptr;     ///< LLM provider（宿主保证存活于会话周期）
    std::shared_ptr<ToolRegistry> sub_registry;  ///< 父会话工具注册表（构建子 Agent 独立工具集）
    IConfigManager* config_manager = nullptr;
    ITaskManager* task_manager = nullptr;
    IEventBus* event_bus = nullptr;
    std::string cwd;
    std::string session_id;  ///< #30：父会话 ID（注入子 Agent ToolContext.session_id，审计关联）
    tool::PermissionMode permission_mode = tool::PermissionMode::Default;
};

/// @brief 启动单个子 Agent 任务
/// @details v1.2.0：并行批量调度复用。为每个任务生成独立 task_id 并投递到线程池，
///          多个子 Agent 并发执行。返回已启动的 Task（供调用方等待/读输出）。
/// @param options 子 Agent 启动参数
/// @return 已启动的 Task
std::shared_ptr<agent::Task> launch_sub_agent(const SubAgentLaunchOptions& options) {
    return options.task_manager->launch(options.task_id,
        [options](const std::atomic<bool>& should_cancel) {
            // v1.1.0：为子 Agent 构建独立工具集（不共享父 registry 的暴露面）
            //  - 白名单过滤：tools 为空使用全部已注册工具，否则仅保留白名单内工具
            //  - Plan 只读：父处于 Plan（只读）时仅保留只读工具，杜绝写/执行能力，
            //    与 check_permissions 形成双重防线（权限逃逸纵深防御）
            //  - 防递归：无论如何排除 Agent 工具本身，子 Agent 不能再启动子 Agent，
            //    杜绝无限嵌套/循环（即使白名单显式包含 "Agent" 也会被忽略）
            auto sub_agent_registry = std::make_shared<ToolRegistry>();
            if (options.sub_registry) {
                std::vector<std::shared_ptr<ITool>> candidates =
                    options.tool_whitelist.empty()
                        ? options.sub_registry->get_all_tools()
                        : [&] {
                            std::vector<std::shared_ptr<ITool>> sel;
                            for (const auto& name : options.tool_whitelist) {
                                if (auto t = options.sub_registry->find_by_name(name)) {
                                    sel.push_back(t);
                                }
                            }
                            return sel;
                        }();
                for (const auto& t : candidates) {
                    if (t->name() == kAgentToolName) {
                        continue;  // 防递归：子 Agent 不携带 Agent 工具
                    }
                    if (options.permission_mode == tool::PermissionMode::Plan && !t->is_read_only()) {
                        continue;  // Plan 只读：跳过写/执行工具
                    }
                    sub_agent_registry->register_tool(t);
                }
            }

            // 子会话：全新消息历史，system_prompt = 任务 prompt
            // #30：注入父会话 ID，使子 Agent 工具调用的审计日志可关联到同一会话
            ReActLoop loop(options.provider, sub_agent_registry, options.config_manager,
                           options.task_manager, options.cwd, options.event_bus,
                           options.session_id);
            // 评审 #1：子 Agent 继承父会话权限模式，避免 Plan 只读边界被绕过
            loop.set_permission_mode(options.permission_mode);
            std::vector<ChatMessage> messages;
            // 独立工具集 schema（子 Agent 的 ToolContext.tool_registry 亦指向独立 registry）
            nlohmann::json tools_schema = sub_agent_registry->get_all_schemas();

            auto task_ptr = options.task_manager->find_task(options.task_id);
            ReActResult result = loop.run(messages, options.prompt, tools_schema,
                should_cancel,
                [options, &task_ptr](const ReActStep& step) {
                    const std::string line = format_step_line(step);
                    if (task_ptr && !line.empty()) {
                        task_ptr->append_output(line);
                    }
                    // v1.2.0 子任务进度流式订阅：每个步骤增量推送结构化进度事件，
                    // 使订阅者按 task_id 实时跟踪子任务进度（无需轮询 TaskOutput）。
                    // 仅作增量通知，不注入父 LLM 上下文。
                    // L-3：空文本步骤不发布进度事件，避免空 content 噪音
                    if (options.event_bus != nullptr && !line.empty()) {
                        options.event_bus->publish_async(SubAgentProgressEvent{
                            .task_id = options.task_id,
                            .step_number = step.step_number,
                            .step_type = step_type_str(step.type),
                            .content = line
                        });
                    }
                });

            // 收尾：错误或最终答案写入输出缓冲
            std::string summary;
            if (result.was_error) {
                summary = std::format("Error: {}", result.error_message);
            } else if (!result.final_answer.empty()) {
                summary = std::format("Final: {}", result.final_answer);
            }
            if (task_ptr && !summary.empty()) {
                task_ptr->append_output(summary);
            }

            // v1.1.0 后台结果自动回送：子 Agent 完成后发布完成事件，
            // 使父会话/UI 等订阅者无需轮询 TaskOutput 即可感知结果。
            // 仅携带 task_id + 结果摘要，不注入父 LLM 上下文（避免刷屏父会话）。
            // 无条件发布（即使摘要为空），保证订阅者总能感知任务收尾（评审 #49 L-5）。
            if (options.event_bus != nullptr) {
                options.event_bus->publish_async(SubAgentCompletedEvent{
                    .task_id = options.task_id,
                    .final_answer = summary,
                    .was_error = result.was_error,
                    .duration_ms = static_cast<double>(result.total_duration_ms)
                });
            }
        });
}

} // namespace

const std::string& AgentTool::name() const {
    static const std::string n{kAgentToolName};
    return n;
}

const std::string& AgentTool::description() const {
    static const std::string d{"Launches a sub-agent to handle a complex task."};
    return d;
}

const std::string& AgentTool::prompt() const {
    static const std::string p{
        "Launches a sub-agent with a specific prompt and tool set. "
        "The sub-agent runs independently and returns its result."
    };
    return p;
}

nlohmann::json AgentTool::input_schema() const {
    // v1.2.0：新增 tasks 数组支持并行批量调度；
    // 提供 tasks 时按数组逐项并行启动子 Agent，否则回退到单个 prompt
    return {
        {"type", "object"},
        {"properties", {
            {"prompt", {{"type", "string"}, {"description", "Single task prompt (used when 'tasks' is omitted)"}}},
            {"tasks", {
                {"type", "array"},
                {"items", {
                    {"type", "object"},
                    {"properties", {
                        {"prompt", {{"type", "string"}, {"description", "Task prompt for one sub-agent"}}},
                        {"tools", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Allowed tools for this task (whitelist; empty uses all registered tools)"}}}
                    }},
                    {"required", {"prompt"}},
                    {"additionalProperties", false}
                }},
                {"description", "Batch of sub-agent tasks to launch in parallel (each gets its own task_id)"}
            }},
            {"tools", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Allowed tools for the single sub-agent (whitelist; empty/omitted uses all registered tools)"}}},
            {"run_in_background", {{"type", "boolean"}, {"description", "Run the sub-agent(s) in background (default true); false waits for completion"}}}
        }},
        {"anyOf", nlohmann::json::array({
            {{"required", nlohmann::json::array({"prompt"})}},
            {{"required", nlohmann::json::array({"tasks"})}}
        })},
        {"additionalProperties", false}
    };
}

ResultV2<ToolResult> AgentTool::call(
    const nlohmann::json& input,
    const ToolContext& ctx
) const {
    // 1. 校验输入
    if (input.is_null() || !input.is_object()) {
        return ResultV2<ToolResult>::err(
            Error::Code::InvalidInput, "Agent: input must be an object");
    }
    const bool run_in_background = input.value("run_in_background", true);

    // v1.1.0：解析顶级 tools 白名单（空/缺失 → 使用全部已注册工具；仅单任务模式生效）
    std::vector<std::string> tool_whitelist;
    if (input.contains("tools") && input["tools"].is_array()) {
        for (const auto& item : input["tools"]) {
            if (item.is_string()) {
                tool_whitelist.push_back(item.get<std::string>());
            }
        }
    }

    // v1.2.0：解析批量任务（tasks 数组 → 并行）或单个 prompt（向后兼容）
    struct SubTaskSpec {
        std::string prompt;
        std::vector<std::string> tools;
    };
    std::vector<SubTaskSpec> specs;
    if (input.contains("tasks") && input["tasks"].is_array() && !input["tasks"].empty()) {
        for (const auto& item : input["tasks"]) {
            if (!item.is_object()) continue;
            const std::string p = item.value("prompt", "");
            if (p.empty()) continue;
            SubTaskSpec spec;
            spec.prompt = p;
            if (item.contains("tools") && item["tools"].is_array()) {
                for (const auto& t : item["tools"]) {
                    if (t.is_string()) spec.tools.push_back(t.get<std::string>());
                }
            }
            specs.push_back(std::move(spec));
        }
    }
    if (specs.empty()) {
        // 单任务模式（向后兼容）
        const std::string prompt = input.value("prompt", "");
        if (prompt.empty()) {
            return ResultV2<ToolResult>::err(
                Error::Code::MissingArgument,
                "Agent: 'prompt' is required (or provide a non-empty 'tasks' array)");
        }
        specs.push_back({prompt, tool_whitelist});
    }

    if (ctx.provider_ptr == nullptr) {
        return ResultV2<ToolResult>::err(
            Error::Code::NotImplemented, "Agent: no LLM provider available");
    }
    if (ctx.task_manager_ptr == nullptr) {
        return ResultV2<ToolResult>::err(
            Error::Code::NotImplemented, "Agent: no task manager available");
    }
    if (ctx.config_manager_ptr == nullptr) {
        return ResultV2<ToolResult>::err(
            Error::Code::NotImplemented, "Agent: no config manager available");
    }

    // 捕获子 Agent 上下文（指针均由调用方保证存活于会话周期）
    ICompletionProvider* provider = ctx.provider_ptr;
    std::shared_ptr<ToolRegistry> sub_registry = ctx.tool_registry;
    IConfigManager* config_manager = ctx.config_manager_ptr;
    ITaskManager* task_manager = ctx.task_manager_ptr;
    IEventBus* event_bus = ctx.event_bus_ptr;
    std::string cwd = ctx.cwd;
    // #26 评审 #1：继承父会话权限模式，防止子 Agent 提升权限（父 Plan 只读时子 Agent 也受限）
    const tool::PermissionMode permission_mode = ctx.permission_mode;

    // 2. 并行启动所有子 Agent（各自独立 task_id，线程池并发执行）
    std::vector<std::shared_ptr<agent::Task>> tasks;
    std::vector<std::string> ids;
    tasks.reserve(specs.size());
    ids.reserve(specs.size());
    for (const auto& spec : specs) {
        const std::string task_id = generate_task_id();
        ids.push_back(task_id);
        tasks.push_back(launch_sub_agent(SubAgentLaunchOptions{
            .task_id = task_id,
            .prompt = spec.prompt,
            .tool_whitelist = spec.tools,
            .provider = provider,
            .sub_registry = sub_registry,
            .config_manager = config_manager,
            .task_manager = task_manager,
            .event_bus = event_bus,
            .cwd = cwd,
            .session_id = ctx.session_id,  // #30：父会话 ID 传递给子 Agent
            .permission_mode = permission_mode
        }));
    }

    // 3. 同步模式：并行等待全部任务完成再返回（M-2：waitForTasks 单一 30s 兜底，
    //    替代逐个 wait 的最坏 N×30s）。超时后返回但任务可能仍在运行，返回时明确提示，
    //    避免工具调用线程无限阻塞（评审 #3）
    if (!run_in_background) {
        task_manager->waitForTasks(tasks);
        const bool all_finished = std::all_of(tasks.begin(), tasks.end(),
            [](const auto& t) { return t->isFinished(); });
        if (!all_finished) {
            std::string partial;
            for (size_t i = 0; i < tasks.size(); ++i) {
                partial += std::format("--- {} ---\n{}\n", ids[i], tasks[i]->output());
            }
            return ResultV2<ToolResult>::ok(ToolResult::ok(std::format(
                "Sub-agent(s) timed out waiting ({}): {}. They may still be running; "
                "use TaskStop to cancel. Partial output:\n{}",
                fmt_task_count(ids.size()), fmt_join_ids(ids), partial)));
        }
        std::string out;
        for (size_t i = 0; i < tasks.size(); ++i) {
            out += std::format("--- {} ---\n{}\n", ids[i], tasks[i]->output());
        }
        return ResultV2<ToolResult>::ok(ToolResult::ok(std::format(
            "Sub-agents completed ({}).\n{}", fmt_task_count(ids.size()), out)));
    }

    // 后台模式：立即返回全部 task_id
    if (ids.size() == 1) {
        return ResultV2<ToolResult>::ok(ToolResult::ok(std::format(
            "Sub-agent launched (task: {}). Use TaskOutput to read its progress.", ids[0])));
    }
    return ResultV2<ToolResult>::ok(ToolResult::ok(std::format(
        "Sub-agents launched ({}): {}. Use TaskOutput to read their progress.",
        fmt_task_count(ids.size()), fmt_join_ids(ids))));
}

} // namespace agent::tool
