#include "agent/core/goal_guarded_agent.h"

#include <atomic>
#include <string>
#include <utility>
#include <vector>

#include "agent/core/verdict.h"
#include "liblogger/logger.h"

namespace agent {

std::unique_ptr<ReActLoop> ReActLoopFactory::make_one_step(const GoalAgentDeps& deps) {
    // max_iterations=1：每次 run() 只推进一轮 Thought+Action+Observation
    // （LLM 无 tool_use 时即给出本轮 FinalAnswer 退出）。外层 GoalGuarded 环
    // 在每轮后跑 Verdict 决定是否续轮，因此本轮是否"FinalAnswer"不影响正确性。
    ReActLoop::Config cfg;
    cfg.max_iterations = 1;
    return std::make_unique<ReActLoop>(
        deps.provider, deps.registry, cfg,
        deps.config_manager, deps.task_manager, deps.cwd,
        deps.external_compactor, deps.event_bus, deps.touch_collector,
        deps.file_index_invalidator, deps.session_id);
}

GoalGuardedAgent::GoalGuardedAgent(GoalAgentDeps deps)
    : m_deps(std::move(deps)) {}

ReActResult GoalGuardedAgent::run(
    std::vector<ChatMessage>& messages,
    const std::string& system_prompt,
    const nlohmann::json& tools_schema,
    const std::atomic<bool>& should_cancel,
    const AgentGoal& goal,
    IReActObserver* observer) {

    // 无目标：退化为单轮 ReAct，直接返回（不循环）
    if (!goal.has_goal()) {
        return ReActLoopFactory::make_one_step(m_deps)->run(
            messages, system_prompt, tools_schema, should_cancel, observer);
    }

    const std::string cwd = m_deps.cwd;
    ReActResult result;             // 累积：last 轮的步骤/token + 最终 final_answer
    bool achieved = false;

    for (int attempt = 1; attempt <= goal.max_attempts; ++attempt) {
        if (should_cancel) {
            result.was_interrupted = true;
            result.error_message = "cancelled by user (goal loop)";
            LOG_WARN("[goal_guarded] cancelled, attempt={}", attempt);
            break;
        }

        // 先验：测试/文件类目标可能初始即达成，避免多余 ReAct
        Verdict pre = check_goal(goal, cwd);
        if (pre.status == GoalStatus::Achieved) {
            result = ReActLoopFactory::make_one_step(m_deps)->run(  // 让 LLM 给结语
                messages, system_prompt, tools_schema, should_cancel, observer);
            result.goal_status = GoalStatus::Achieved;  // 结果回填
            achieved = true;
            LOG_INFO("[goal_guarded] achieved on pre-check, attempt={}", attempt);
            break;
        }

        // 跑一轮 ReAct（LLM 分析 + 行动）
        ReActResult step = ReActLoopFactory::make_one_step(m_deps)->run(
            messages, system_prompt, tools_schema, should_cancel, observer);
        result = step;  // 保留最后一轮（历史/token/步骤都挂到 result）
        if (step.was_interrupted || step.was_error) {
            // 流式错误/中断：不重试，向上透传错误状态
            LOG_WARN("[goal_guarded] step error/interrupt, attempt={}", attempt);
            break;
        }

        // 观察后验证（关键：即使 LLM 主观 FinalAnswer 也算未达成）
        Verdict post = check_goal(goal, cwd);
        if (post.status == GoalStatus::Achieved) {
            achieved = true;
            LOG_INFO("[goal_guarded] achieved after action, attempt={}", attempt);
            break;
        }

        // 未达成：向消息注入"继续"提示，让下一轮知道目标仍存
        messages.push_back(ChatMessage::user(std::format(
            "[goal] 上一轮行动后目标尚未达成（{}）。请继续修复/执行，直到达成",
            post.detail)));

        // 可选：每 N 次未达成弹 AskUser（本版由宿主通过外部中断注入，此处仅记录）
        if (goal.ask_user_every > 0 && attempt % goal.ask_user_every == 0) {
            LOG_DEBUG("[goal_guarded] ask_user checkpoint, attempt={}", attempt);
        }
    }

    if (!achieved) {
        result.goal_status = GoalStatus::Failed;
        result.was_error = true;
        if (result.error_message.empty()) {
            result.error_message = std::format(
                "goal not achieved after {} attempts", goal.max_attempts);
        }
        LOG_WARN("[goal_guarded] not achieved after max_attempts={}, status={}",
                 goal.max_attempts, static_cast<int>(result.goal_status));
    } else {
        result.goal_status = GoalStatus::Achieved;
    }

    return result;
}

} // namespace agent