/**
 * @file agent_loop_adapters.h
 * @brief IAgentLoop 适配器（#33 类型体系 / 里程碑 0.6.x）
 * @details 将现有 ReActLoop（保留自身 run() 零改动）与 GoalGuardedAgent
 *          包装为统一 IAgentLoop 接口，供 QueryEngine 按 AgentType 路由调用。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "agent/core/i_agent_loop.h"
#include "agent/core/react_loop.h"
#include "agent/core/goal_guarded_agent.h"

namespace agent {

/// @brief 现有 ReActLoop 的 IAgentLoop 适配（默认通用对话）
/// @details 持有 ReActLoop 实例（由 QueryEngine 按 Celia deps 构造），
///          包装其 run() 为 IAgentLoop::run()。agent_type = ReAct。
class WORKX_API ReActLoopAdapter final : public IAgentLoop {
public:
    explicit ReActLoopAdapter(std::unique_ptr<ReActLoop> loop);
    AgentRunResult run(AgentRunContext ctx) override;
    AgentType type() const noexcept override { return AgentType::ReAct; }

private:
    std::unique_ptr<ReActLoop> m_loop;
};

/// @brief GoalGuardedAgent 的 IAgentLoop 适配（目标驱动）
/// @details agent_type = GoalGuarded；仅当 ctx.goal 非空时才进入目标环，
///          否则退化为单轮 ReAct（由 GoalGuardedAgent 自身处理）。
class WORKX_API GoalGuardedLoopAdapter final : public IAgentLoop {
public:
    explicit GoalGuardedLoopAdapter(GoalAgentDeps deps);
    AgentRunResult run(AgentRunContext ctx) override;
    AgentType type() const noexcept override { return AgentType::GoalGuarded; }

private:
    GoalAgentDeps m_deps;
};

} // namespace agent