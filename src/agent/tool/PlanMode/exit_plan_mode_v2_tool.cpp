/**
 * @file exit_plan_mode_v2_tool.cpp
 * @brief ExitPlanModeV2Tool 实现（#28）
 * @details 流程：
 *          1. 复用 ask_user_confirm 把方案呈现给用户（Yes/No）
 *          2. 批准 → 权限模式切回 Default（若当前为 Plan；Bypass 不降级）
 *          3. 发布 ExitPlanModeEvent（含批准结果）
 *          4. 无确认通道（event_bus 为空）→ fail-closed 视为未批准
 * @version 1.0.0
 * @date 2026-08
 */

#include "agent/tool/PlanMode/exit_plan_mode_v2_tool.h"

#include <format>

#include "agent/tool/permission_ask.h"
#include "core/events/agent_events.h"
#include "core/events/i_event_bus.h"

namespace agent::tool {

const std::string& ExitPlanModeV2Tool::name() const {
    static const std::string n{"ExitPlanModeV2"};
    return n;
}

const std::string& ExitPlanModeV2Tool::description() const {
    static const std::string d{
        "Exit plan mode and present the plan to the user for approval. "
        "Requires the 'plan' field with the detailed plan (files to change, "
        "reasons, risks). After approval, write/edit/execute tools are allowed again."};
    return d;
}

const std::string& ExitPlanModeV2Tool::prompt() const {
    static const std::string p{
        "After researching in plan mode, call ExitPlanModeV2 with a complete plan: "
        "which files to change, why, and risks. The user must approve before you "
        "start editing. If rejected, adjust the plan and try again."};
    return p;
}

nlohmann::json ExitPlanModeV2Tool::input_schema() const {
    return {
        {"type", "object"},
        {"properties", nlohmann::json::object({
            {"plan", {
                {"type", "string"},
                {"description", "The detailed plan: files to change, approach, risks."}
            }}
        })},
        {"required", nlohmann::json::array({"plan"})},
        {"additionalProperties", false}
    };
}

ResultV2<ToolResult> ExitPlanModeV2Tool::call(
    const nlohmann::json& input,
    const ToolContext& ctx
) const {
    if (!input.contains("plan") || !input["plan"].is_string()) {
        return ResultV2<ToolResult>::err(
            Error::Code::MissingArgument, "ExitPlanModeV2: 'plan' is required");
    }
    const std::string plan = input["plan"].get<std::string>();
    if (plan.empty()) {
        return ResultV2<ToolResult>::err(
            Error::Code::InvalidInput, "ExitPlanModeV2: 'plan' must not be empty");
    }

    // 1. 呈现方案请求用户批准（无确认通道 → fail-closed 未批准）
    const std::string question = std::format(
        "Plan:\n\n```\n{}\n```\n\nApprove this plan and exit plan mode?", plan);
    const bool approved = ask_user_confirm(ctx, question);

    // 2. 批准 → 恢复进入计划前的原模式（评审 #1：非硬编码回 Default）
    //    Bypass 未进入 Plan，无需切换；拒绝则保持 Plan
    if (approved && is_plan_mode(ctx.permission_mode)) {
        if (ctx.on_exit_plan_mode) {
            ctx.on_exit_plan_mode();
        } else {
            ctx.set_permission_mode(PermissionMode::Default);
        }
    }

    // 3. 发布退出计划模式事件（publish_async 传值，typeid 匹配订阅）
    if (ctx.event_bus_ptr) {
        ctx.event_bus_ptr->publish_async(ExitPlanModeEvent{
            .session_id = ctx.session_id,
            .plan = plan,
            .approved = approved
        });
    }

    if (approved) {
        return ResultV2<ToolResult>::ok(ToolResult::ok(std::string{
            "Plan approved. Exiting plan mode; write/edit and command execution are allowed again."}));
    }
    return ResultV2<ToolResult>::ok(ToolResult::ok(std::string{
        "Plan not approved. Staying in plan mode. Revise the plan and call ExitPlanModeV2 again."}));
}

} // namespace agent::tool