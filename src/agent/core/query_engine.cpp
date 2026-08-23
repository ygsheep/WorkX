#include "agent/core/query_engine.h"

#include <utility>

#include "agent/core/agent_loop_adapters.h"
#include "agent/config/app_config.h"        // keys::AGENT_ACTIVE
#include "core/config/i_config_manager.h"   // get_or<std::string>
#include "liblogger/logger.h"

namespace agent {

QueryEngine::QueryEngine(GoalAgentDeps deps)
    : m_deps(std::move(deps)) {}

AgentType QueryEngine::resolve_agent_type(const IConfigManager& cfg) const {
    const std::string active = cfg.get_or<std::string>(agent::keys::AGENT_ACTIVE, "");
    return parse_agent_type(active);
}

std::unique_ptr<IAgentLoop> QueryEngine::make_loop(AgentType type) const {
    switch (type) {
        case AgentType::ReAct: {
            // 常规全量 ReAct 循环（默认 max_iterations），非单步
            auto loop = std::make_unique<ReActLoop>(
                m_deps.provider, m_deps.registry, ReActLoop::Config{},
                m_deps.config_manager, m_deps.task_manager, m_deps.cwd,
                m_deps.external_compactor, m_deps.event_bus, m_deps.touch_collector,
                m_deps.file_index_invalidator, m_deps.session_id);
            return std::make_unique<ReActLoopAdapter>(std::move(loop));
        }
        case AgentType::GoalGuarded: {
            return std::make_unique<GoalGuardedLoopAdapter>(m_deps);
        }
        case AgentType::Unknown:
        case AgentType::Planner:
        case AgentType::Executor:
        case AgentType::Coordinator:
        case AgentType::Researcher:
        case AgentType::Reviewer:
        case AgentType::Batch:
        case AgentType::Watch:
        default:
            LOG_WARN("[query_engine] agent type '{}' not implemented, fallback to ReAct",
                     to_string(type));
            // 未实现类型回退到 ReAct（占位，后续实现可扩展）
            auto loop = std::make_unique<ReActLoop>(
                m_deps.provider, m_deps.registry, ReActLoop::Config{},
                m_deps.config_manager, m_deps.task_manager, m_deps.cwd,
                m_deps.external_compactor, m_deps.event_bus, m_deps.touch_collector,
                m_deps.file_index_invalidator, m_deps.session_id);
            return std::make_unique<ReActLoopAdapter>(std::move(loop));
    }
}

AgentRunResult QueryEngine::run(const IConfigManager& cfg, AgentRunContext ctx) {
    AgentType type = resolve_agent_type(cfg);
    auto loop = make_loop(type);
    if (!loop) {
        // 理论不可达（make_loop 恒返回非空），防呆回退
        loop = make_loop(AgentType::ReAct);
    }
    AgentRunResult out = loop->run(std::move(ctx));
    // 覆盖为实际执行的类型（回退场景下准确）
    out.agent_type = loop->type();
    return out;
}

} // namespace agent