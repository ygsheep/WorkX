/**
 * @file agent_tool.cpp
 * @brief AgentTool 实现
 * @details 子 Agent 调度工具的具体实现
 * @version 1.0.0
 * @date 2026-07
 */

#include "agent/tool/AgentTool/agent_tool.h"

#include <random>
#include <format>

#include "agent/api/chat_types.h"
#include "agent/core/react_loop.h"
#include "core/task/task_manager.h"
#include "core/utils/error.h"

namespace agent::tool {

namespace {

/// @brief 生成任务 id（对齐 TS generateTaskId：'a' 前缀 + 8 个随机小写字母数字）
std::string generate_task_id() {
    static constexpr char kAlphabet[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    std::random_device rd;
    std::uniform_int_distribution<size_t> dist(0, sizeof(kAlphabet) - 2);
    std::string id = "a";
    id.reserve(9);
    for (int i = 0; i < 8; ++i) {
        id += kAlphabet[dist(rd)];
    }
    return id;
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

} // namespace

const std::string& AgentTool::name() const {
    static const std::string n{"Agent"};
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
    return {
        {"type", "object"},
        {"properties", {
            {"prompt", {{"type", "string"}, {"description", "The task prompt for the sub-agent"}}},
            {"tools", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Allowed tools for the sub-agent (reserved)"}}},
            {"run_in_background", {{"type", "boolean"}, {"description", "Run the sub-agent in background (default true); false runs synchronously"}}}
        }},
        {"required", {"prompt"}},
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
    const std::string prompt = input.value("prompt", "");
    if (prompt.empty()) {
        return ResultV2<ToolResult>::err(
            Error::Code::MissingArgument, "Agent: 'prompt' is required");
    }
    const bool run_in_background = input.value("run_in_background", true);

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

    // 2. 生成任务 id 并启动子 Agent
    const std::string task_id = generate_task_id();

    // 捕获子 Agent 上下文（指针均由调用方保证存活于会话周期）
    ICompletionProvider* provider = ctx.provider_ptr;
    std::shared_ptr<ToolRegistry> sub_registry = ctx.tool_registry;
    IConfigManager* config_manager = ctx.config_manager_ptr;
    ITaskManager* task_manager = ctx.task_manager_ptr;
    IEventBus* event_bus = ctx.event_bus_ptr;
    std::string cwd = ctx.cwd;
    // #26 评审 #1：继承父会话权限模式，防止子 Agent 提升权限（父 Plan 只读时子 Agent 也受限）
    const tool::PermissionMode permission_mode = ctx.permission_mode;

    auto task = task_manager->launch(task_id,
        [task_id, provider, sub_registry, config_manager, task_manager,
         event_bus, cwd, prompt, permission_mode](const std::atomic<bool>& should_cancel) {
            // 子会话：全新消息历史，system_prompt = 任务 prompt
            ReActLoop loop(provider, sub_registry, config_manager,
                           task_manager, cwd, event_bus);
            // 评审 #1：子 Agent 继承父会话权限模式，避免 Plan 只读边界被绕过
            loop.set_permission_mode(permission_mode);
            std::vector<ChatMessage> messages;
            nlohmann::json tools_schema = sub_registry
                ? sub_registry->get_all_schemas()
                : nlohmann::json::array();

            auto task_ptr = task_manager->find_task(task_id);
            ReActResult result = loop.run(messages, prompt, tools_schema,
                should_cancel,
                [&task_ptr](const ReActStep& step) {
                    const std::string line = format_step_line(step);
                    if (task_ptr && !line.empty()) {
                        task_ptr->append_output(line);
                    }
                });

            // 收尾：错误或最终答案写入输出缓冲
            if (task_ptr) {
                if (result.was_error) {
                    task_ptr->append_output(std::format("Error: {}", result.error_message));
                } else if (!result.final_answer.empty()) {
                    task_ptr->append_output(std::format("Final: {}", result.final_answer));
                }
            }
        });

    // 同步模式：等待任务完成再返回。TaskManager::wait 内部已带 30s 兜底超时；
    // 超时后返回但任务可能仍在运行，返回时明确提示，避免工具调用线程无限阻塞
    // （评审 #3）
    if (!run_in_background) {
        task_manager->wait(task);
        if (!task->isFinished()) {
            return ResultV2<ToolResult>::ok(ToolResult::ok(std::format(
                "Sub-agent timed out waiting (task: {}). It may still be running; "
                "use TaskStop to cancel it. Partial output:\n{}",
                task_id, task->output())));
        }
        return ResultV2<ToolResult>::ok(ToolResult::ok(std::format(
            "Sub-agent completed (task: {}).\n{}", task_id, task->output())));
    }

    return ResultV2<ToolResult>::ok(ToolResult::ok(std::format(
        "Sub-agent launched (task: {}). Use TaskOutput to read its progress.", task_id)));
}

} // namespace agent::tool
