/**
 * @file goal_guarded_agent.h
 * @brief GoalGuardedAgent — 目标导向 Agent（#31 / 里程碑 0.6.x）
 * @details 以"目标达成"为终止条件的 Agent 模式，包装现有 ReActLoop：
 *          每轮走一次 ReAct（Thought/Action/Observation），随后执行目标验证
 *          （check_goal）。达成 → 成功退出；未达成 → 注入"继续"提示并续一轮，
 *          直至达成或达到 max_attempts（失败退出、汇报当前状态）。
 *
 *          区别于普通 ReActLoop 的关键：LLM 主观的 FinalAnswer 不能作为
 *          成功信号——GoalGuarded 强制在每轮后跑 Verdict，未达成时即使 LLM
 *          已给出"我修好了"也会续轮，杜绝"自欺"（issue #31 场景 2）。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "agent/core/react_loop.h"        // IReActObserver / ReActResult / CacheAwareCompactor
#include "agent/core/goal_verdict.h"      // AgentGoal
#include "agent/api/i_completion_provider.h"
#include "agent/tool/registry.h"
#include "core/events/event_bus.h"

namespace agent {

// 前向声明（与 react_loop.h 一致，D-5：避免头文件强依赖）
class IConfigManager;
class ITaskManager;
namespace skill { class TouchCollector; }

/// @brief GoalGuardedAgent 依赖注入（对齐 ReActLoop 构造参数，避免重复参数列表）
/// @details ReActLoop 每轮由本 Agent 新建（Config{max_iterations=1}），故所有
///          依赖需在本 Agent 生命周期内保持有效（含 compactor 跨轮持久）。
struct GoalAgentDeps {
    ICompletionProvider* provider = nullptr;
    std::shared_ptr<tool::ToolRegistry> registry;
    IConfigManager* config_manager = nullptr;
    ITaskManager* task_manager = nullptr;
    std::string cwd;
    /// 跨轮持久化的压缩器（nullptr 则 ReActLoop 内部新建，仅轮内有效）
    CacheAwareCompactor* external_compactor = nullptr;
    IEventBus* event_bus = nullptr;
    skill::TouchCollector* touch_collector = nullptr;
    std::function<void()> file_index_invalidator;
    std::string session_id;
};

/// @brief 构造 GoalGuardedAgent 内部 ReActLoop 的工厂（供 QueryEngine 复用注入逻辑）
/// @details 注入途径与 ChatSession 构造 ReActLoop 完全一致；max_iterations=1 使每次
///          run() 只推进一轮 Thought+Action+Observation，由外层环控制续轮。
struct ReActLoopFactory {
    static std::unique_ptr<ReActLoop> make_one_step(const GoalAgentDeps& deps);
};

/// @brief 目标导向 Agent
class WORKX_API GoalGuardedAgent {
public:
    explicit GoalGuardedAgent(GoalAgentDeps deps);

    /// @brief 以目标达成（或超限）为终止条件运行
    /// @param messages 会话历史（跨轮累积，写入 assistant/tool/提示消息）
    /// @param system_prompt 系统提示词
    /// @param tools_schema 工具 schema
    /// @param should_cancel 外部取消信号
    /// @param goal 目标定义（None 时退化为一轮 ReAct）
    /// @param observer 事件观察者（可选）
    /// @return 结果（含 goal_status 与最终 final_answer）
    ReActResult run(std::vector<ChatMessage>& messages,
                    const std::string& system_prompt,
                    const nlohmann::json& tools_schema,
                    const std::atomic<bool>& should_cancel,
                    const AgentGoal& goal,
                    IReActObserver* observer = nullptr);

private:
    GoalAgentDeps m_deps;
};

} // namespace agent