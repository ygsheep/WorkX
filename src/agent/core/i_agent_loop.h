/**
 * @file i_agent_loop.h
 * @brief IAgentLoop — 统一 Agent 循环接口（#33 类型体系 / 里程碑 0.6.x）
 * @details 屏蔽不同 Agent 类型（ReAct / GoalGuarded / Planner / Coordinator …）
 *          的执行差异，供 QueryEngine 统一调用。所有实现都复用 ReActResult
 *          承载步骤历史/token/目标状态，UI 侧无需感知 Agent 类型差异。
 *
 *          现有 ReActLoop 保留其自有的 run() 主接口（向后兼容、零改动），
 *          本接口通过适配器包装暴露；新 Agent 类型直接实现 IAgentLoop。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <functional>
#include <memory>
#include <atomic>

#include "agent/core/agent_type.h"
#include "agent/core/react_loop.h"      // ReActResult / ChatMessage
#include "agent/core/goal_verdict.h"    // AgentGoal

namespace agent {

/// @brief 一次 Agent 循环的输入（与 ChatSession run_completion 对齐）
struct AgentRunContext {
    std::vector<ChatMessage>* messages = nullptr;   ///< 会话历史（读写）
    std::string system_prompt;
    nlohmann::json tools_schema;
    const std::atomic<bool>* should_cancel = nullptr;
    AgentGoal goal;                                ///< 空 = 无目标守卫
    IReActObserver* observer = nullptr;
};

/// @brief 一次 Agent 循环的结果
struct AgentRunResult {
    ReActResult react;            ///< 复用既有结果载体（步骤/token/目标状态）
    AgentType agent_type = AgentType::Unknown;  ///< 实际执行的类型
};

/// @brief 统一 Agent 循环接口
class WORKX_API IAgentLoop {
public:
    virtual ~IAgentLoop() = default;

    /// @brief 执行一轮 Agent 循环
    virtual AgentRunResult run(AgentRunContext ctx) = 0;

    /// @brief 本 Agent 类型
    virtual AgentType type() const noexcept = 0;
};

/// @brief 目标注入来源（QueryEngine 解析 agent.active 后，目标从何而来）
struct GoalResolver {
    /// 根据类型/配置产出目标；返回 None 表示无目标（普通对话）
    std::function<AgentGoal()> resolve;
};

/// @brief 永不触发的取消信号（adaptive 需要非空 should_cancel 时使用）
/// @details 返回静态原子的引用，生命周期同进程；用 == false 比对无副作用。
const std::atomic<bool>& kNeverCancel();

} // namespace agent