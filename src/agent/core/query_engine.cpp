#include "agent/core/query_engine.h"

#include <utility>

#include "agent/core/agent_loop_adapters.h"
#include "agent/config/app_config.h"        // keys::AGENT_ACTIVE
#include "core/config/i_config_manager.h"   // get_or<std::string>
#include "liblogger/logger.h"

namespace agent {

QueryEngine::QueryEngine(GoalAgentDeps deps)
    : m_deps(std::move(deps)) {}

void QueryEngine::set_permission(PermissionSnapshot snap) {
    m_deps.permission_mode = snap.mode;
    m_deps.permission_mode_before_plan = snap.before_plan;
    m_deps.permission_state_changed_cb = std::move(snap.on_changed);
}

void QueryEngine::apply_permission(std::unique_ptr<ReActLoop>& loop) const {
    loop->apply_permission_state(
        m_deps.permission_mode, m_deps.permission_mode_before_plan,
        m_deps.permission_mode == tool::PermissionMode::Plan);
    if (m_deps.permission_state_changed_cb) {
        loop->set_permission_state_changed_callback(m_deps.permission_state_changed_cb);
    }
}

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
            apply_permission(loop);
            return std::make_unique<ReActLoopAdapter>(std::move(loop));
        }
        case AgentType::GoalGuarded: {
            // 权限/回调随 m_deps 透传给适配器 → GoalGuardedAgent 内部循环
            return std::make_unique<GoalGuardedLoopAdapter>(m_deps);
        }
        case AgentType::Script:
            // #32：确定性脚本执行（无 LLM），复用同一套 deps
            return std::make_unique<ScriptLoopAdapter>(m_deps);
        case AgentType::Batch:
            // #32：glob 同构并行（无 LLM）
            return std::make_unique<BatchLoopAdapter>(m_deps);
        case AgentType::Watch:
            // #32：文件/事件监控轮询（无 LLM）
            return std::make_unique<WatchLoopAdapter>(m_deps);
        case AgentType::Planner:
        case AgentType::Executor:
        case AgentType::Coordinator:
        case AgentType::Researcher:
        case AgentType::Reviewer:
            // #33：角色 Agent（提示覆盖 + 工具过滤）+ 复用 ReActLoop 引擎
            return std::make_unique<RoleLoopAdapter>(m_deps, type);
        case AgentType::Unknown:
        default:
            LOG_WARN("[query_engine] agent type '{}' not implemented, fallback to ReAct",
                     to_string(type));
            // 未实现类型回退到 ReAct（占位，后续实现可扩展）
            auto loop = std::make_unique<ReActLoop>(
                m_deps.provider, m_deps.registry, ReActLoop::Config{},
                m_deps.config_manager, m_deps.task_manager, m_deps.cwd,
                m_deps.external_compactor, m_deps.event_bus, m_deps.touch_collector,
                m_deps.file_index_invalidator, m_deps.session_id);
            apply_permission(loop);
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

    // queryTracking 调用链：注入 tracker（GoalGuarded 记录每轮 Verdict）+ 启动本轮记录
    m_deps.tracker = &m_tracker;
    m_tracker.begin(type, ctx.goal_spec);

    AgentRunResult out = loop->run(std::move(ctx));
    // 覆盖为实际执行的类型（回退场景下准确）
    out.agent_type = loop->type();
    m_tracker.finish(out.react.goal_status, out.react.final_answer);
    return out;
}

} // namespace agent