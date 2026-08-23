/**
 * @file query_engine.h
 * @brief QueryEngine — Agent 会话生命周期编排器（里程碑 0.6.x）
 * @details 从注释 stub 落为实体：作为唯一编排入口，按 agent.active 解析
 *          AgentType，构造对应 IAgentLoop（ReAct 适配器 / GoalGuarded 适配器），
 *          注入统一下游依赖并执行一轮查询。职责收拢自 ChatSession::run_completion
 *          的"建 loop 段"，使新 Agent 类型只需实现 IAgentLoop 即可接入。
 * @version 1.1.0
 * @date 2026-08
 */

#pragma once

#include <memory>
#include <string>

#include "agent/core/agent_type.h"
#include "agent/core/goal_guarded_agent.h"  // GoalAgentDeps / ReActLoopFactory
#include "agent/core/i_agent_loop.h"
#include "core/export.h"

namespace agent {

class IConfigManager;

/// @brief 查询引擎：解析 AgentType → 构造 Agent → 执行
class WORKX_API QueryEngine {
public:
    /// @brief 依赖（与 GoalAgentDeps 同源，ReAct 与 GoalGuarded 共用）
    explicit QueryEngine(GoalAgentDeps deps);

    /// @brief agent.active → AgentType（含 skill 过滤兼容，见 parse_agent_type）
    AgentType resolve_agent_type(const IConfigManager& cfg) const;

    /// @brief 构造并注册一种 Agent 类型的执行器（未实现/未知返回 nullptr）
    /// @param type 目标类型（ReAct / GoalGuarded 落地；其余返回 nullptr 占位）
    std::unique_ptr<IAgentLoop> make_loop(AgentType type) const;

    /// @brief 便捷：按配置解析类型并执行一轮查询
    /// @param cfg 配置管理器（读 agent.active）
    /// @param ctx 查询上下文（含目标与观察者）
    /// @return 执行结果；类型未知回退 ReAct
    AgentRunResult run(const IConfigManager& cfg, AgentRunContext ctx);

private:
    GoalAgentDeps m_deps;
};

} // namespace agent