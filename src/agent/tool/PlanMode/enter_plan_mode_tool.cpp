/**
 * @file enter_plan_mode_tool.cpp
 * @brief EnterPlanModeTool 实现（#28）
 * @version 1.0.0
 * @date 2026-08
 */

#include "agent/tool/PlanMode/enter_plan_mode_tool.h"

#include <format>

#include "core/events/agent_events.h"
#include "core/events/i_event_bus.h"

namespace agent::tool {

const std::string& EnterPlanModeTool::name() const {
    static const std::string n{"EnterPlanMode"};
    return n;
}

const std::string& EnterPlanModeTool::description() const {
    static const std::string d{
        "Enter plan mode. In plan mode, write/edit/execute tools are blocked; "
        "you can only read and research. Use this for large or uncertain tasks "
        "before making changes. Exit with ExitPlanModeV2 after presenting the plan."};
    return d;
}

const std::string& EnterPlanModeTool::prompt() const {
    static const std::string p{
        "Use EnterPlanMode for large or uncertain tasks: first research the codebase "
        "(read-only), then call ExitPlanModeV2 with a detailed plan for user approval. "
        "This avoids wasted edits before the direction is confirmed."};
    return p;
}

nlohmann::json EnterPlanModeTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties", nlohmann::json::object({
            {"reason", {
                {"type", "string"},
                {"description", "Why you are entering plan mode (optional)."}
            }}
        })},
        {"required", nlohmann::json::array()},
        {"additionalProperties", false}
    };
}

ResultV2<ToolResult> EnterPlanModeTool::call(
    const nlohmann::json& input,
    const ToolContext& ctx
) const {
    const std::string reason = input.value("reason", "");

    // 1. 通过回调切换会话权限模式为 Plan（无回调则仅提示）
    ctx.set_permission_mode(PermissionMode::Plan);

    // 2. 发布进入计划模式事件（宿主展示状态）
    // 注意：publish_async 按 T=EnterPlanModeEvent 推导（typeid 匹配订阅），传值而非 shared_ptr
    if (ctx.event_bus_ptr) {
        ctx.event_bus_ptr->publish_async(EnterPlanModeEvent{
            .session_id = ctx.session_id,
            .reason = reason
        });
    }

    std::string msg = "Entered plan mode. Write/edit and command execution are now blocked.";
    if (!reason.empty()) {
        msg += "\nReason: " + reason;
    }
    return ResultV2<ToolResult>::ok(ToolResult::ok(std::move(msg)));
}

} // namespace agent::tool