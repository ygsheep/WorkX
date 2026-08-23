#include "agent/core/agent_loop_adapters.h"

namespace agent {

ReActLoopAdapter::ReActLoopAdapter(std::unique_ptr<ReActLoop> loop)
    : m_loop(std::move(loop)) {}

AgentRunResult ReActLoopAdapter::run(AgentRunContext ctx) {
    AgentRunResult out;
    out.agent_type = type();
    out.react = m_loop->run(
        *ctx.messages, ctx.system_prompt, ctx.tools_schema,
        ctx.should_cancel ? *ctx.should_cancel : kNeverCancel(),
        ctx.observer);
    return out;
}

GoalGuardedLoopAdapter::GoalGuardedLoopAdapter(GoalAgentDeps deps)
    : m_deps(std::move(deps)) {}

AgentRunResult GoalGuardedLoopAdapter::run(AgentRunContext ctx) {
    AgentRunResult out;
    out.agent_type = type();
    GoalGuardedAgent agent(m_deps);
    out.react = agent.run(
        *ctx.messages, ctx.system_prompt, ctx.tools_schema,
        ctx.should_cancel ? *ctx.should_cancel : kNeverCancel(),
        ctx.goal, ctx.goal_spec, ctx.observer);
    return out;
}

const std::atomic<bool>& kNeverCancel() {
    static const std::atomic<bool> k{false};
    return k;
}

} // namespace agent