/**
 * @file test_plan_mode_tools.cpp
 * @brief #28 EnterPlanMode/ExitPlanModeV2 工具单元测试
 * @details 覆盖：
 *          - EnterPlanModeTool：模式回调切换 Plan、发布 EnterPlanModeEvent、无回调容错
 *          - ExitPlanModeV2Tool：参数校验、批准路径（回调回 Default + 事件 approved）、
 *            拒绝路径（保持 Plan）、fail-closed（无 event_bus）、Bypass 不降级
 */

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>
#include <vector>

#include "agent/tool/PlanMode/enter_plan_mode_tool.h"
#include "agent/tool/PlanMode/exit_plan_mode_v2_tool.h"
#include "agent/tool/context.h"
#include "core/config/config_manager.h"
#include "core/events/agent_events.h"
#include "helpers/mock_event_bus.h"

using namespace agent;
using namespace agent::tool;
using namespace agent::test;

namespace {

void fill_ctx(ToolContext& ctx, agent::test::MockEventBus& bus) {
    ctx.cwd = ".";
    ctx.session_id = "test-session";
    ctx.config_manager_ptr = &ConfigManager::instance();
    ctx.event_bus_ptr = &bus;
}

/// 订阅 AskUserRequestEvent 并自动回填结果（模拟 TUI 确认面板）
/// @details 需要 dispatch_enabled + async_auto_flush：工具 call 阻塞等待
///          result_promise 时，发布即同步派发到订阅回调并回填。
void auto_answer(agent::test::MockEventBus& bus, bool yes) {
    bus.set_dispatch_enabled(true);
    bus.set_async_auto_flush(true);
    bus.subscribe<AskUserRequestEvent>(
        [yes](const AskUserRequestEvent& e) {
            AskUserResult result;
            result.submitted = true;
            result.answers.emplace_back("permission", yes ? "Yes" : "No");
            e.result_promise->set_value(result);
        });
}

} // namespace

// ============================================================
// EnterPlanModeTool
// ============================================================

TEST_CASE("EnterPlanModeTool switches mode to Plan via callback", "[tool][plan_mode]") {
    MockEventBus bus;
    ToolContext ctx; fill_ctx(ctx, bus);
    PermissionMode captured = PermissionMode::Default;
    bool callback_called = false;
    ctx.on_permission_mode_changed = [&](PermissionMode m) {
        callback_called = true;
        captured = m;
    };

    EnterPlanModeTool tool;
    auto res = tool.call(R"({"reason": "refactor"})"_json, ctx);
    REQUIRE(res.is_ok());
    REQUIRE(callback_called);
    REQUIRE(captured == PermissionMode::Plan);
}

TEST_CASE("EnterPlanModeTool publishes EnterPlanModeEvent", "[tool][plan_mode]") {
    MockEventBus bus;
    ToolContext ctx; fill_ctx(ctx, bus);

    EnterPlanModeTool tool;
    auto res = tool.call(R"({"reason": "refactor"})"_json, ctx);
    REQUIRE(res.is_ok());
    bus.process_async_events();
    REQUIRE(bus.published_count<EnterPlanModeEvent>() == 1);
}

TEST_CASE("EnterPlanModeTool tolerant without callback", "[tool][plan_mode]") {
    MockEventBus bus;
    ToolContext ctx; fill_ctx(ctx, bus);
    ctx.on_permission_mode_changed = nullptr;  // 未接线

    EnterPlanModeTool tool;
    auto res = tool.call(R"({})"_json, ctx);
    REQUIRE(res.is_ok());
}

// ============================================================
// ExitPlanModeV2Tool
// ============================================================

TEST_CASE("ExitPlanModeV2Tool requires plan field", "[tool][plan_mode]") {
    MockEventBus bus;
    ToolContext ctx; fill_ctx(ctx, bus);

    ExitPlanModeV2Tool tool;
    auto res = tool.call(R"({})"_json, ctx);
    REQUIRE(res.is_err());
    REQUIRE(res.error().code == Error::Code::MissingArgument);
}

TEST_CASE("ExitPlanModeV2Tool approved restores Default mode", "[tool][plan_mode]") {
    MockEventBus bus;
    auto_answer(bus, true);
    ToolContext ctx; fill_ctx(ctx, bus);
    ctx.permission_mode = PermissionMode::Plan;
    PermissionMode captured = PermissionMode::Plan;
    bool callback_called = false;
    ctx.on_permission_mode_changed = [&](PermissionMode m) {
        callback_called = true;
        captured = m;
    };

    ExitPlanModeV2Tool tool;
    auto res = tool.call(R"({"plan": "refactor src/x.cpp"})"_json, ctx);
    REQUIRE(res.is_ok());
    REQUIRE(callback_called);
    REQUIRE(captured == PermissionMode::Default);

    bus.process_async_events();
    REQUIRE(bus.published_count<ExitPlanModeEvent>() == 1);
}

TEST_CASE("ExitPlanModeV2Tool approved event carries plan and approval", "[tool][plan_mode]") {
    MockEventBus bus;
    auto_answer(bus, true);
    ToolContext ctx; fill_ctx(ctx, bus);
    ctx.permission_mode = PermissionMode::Plan;

    std::string captured_plan;
    bool captured_approved = false;
    bus.subscribe<ExitPlanModeEvent>([&](const ExitPlanModeEvent& e) {
        captured_plan = e.plan;
        captured_approved = e.approved;
    });

    ExitPlanModeV2Tool tool;
    auto res = tool.call(R"({"plan": "plan-text"})"_json, ctx);
    REQUIRE(res.is_ok());
    bus.process_async_events();
    REQUIRE(captured_plan == "plan-text");
    REQUIRE(captured_approved);
}

TEST_CASE("ExitPlanModeV2Tool declined stays in plan mode", "[tool][plan_mode]") {
    MockEventBus bus;
    auto_answer(bus, false);
    ToolContext ctx; fill_ctx(ctx, bus);
    ctx.permission_mode = PermissionMode::Plan;
    bool callback_called = false;
    ctx.on_permission_mode_changed = [&](PermissionMode) { callback_called = true; };

    ExitPlanModeV2Tool tool;
    auto res = tool.call(R"({"plan": "plan-text"})"_json, ctx);
    REQUIRE(res.is_ok());
    REQUIRE_FALSE(callback_called);  // 保持 Plan

    bus.process_async_events();
    REQUIRE(bus.published_count<ExitPlanModeEvent>() == 1);
}

TEST_CASE("ExitPlanModeV2Tool fails closed without event bus", "[tool][plan_mode]") {
    ToolContext ctx;
    ctx.cwd = ".";
    ctx.config_manager_ptr = &ConfigManager::instance();
    ctx.event_bus_ptr = nullptr;  // 无确认通道
    ctx.permission_mode = PermissionMode::Plan;
    bool callback_called = false;
    ctx.on_permission_mode_changed = [&](PermissionMode) { callback_called = true; };

    ExitPlanModeV2Tool tool;
    auto res = tool.call(R"({"plan": "plan-text"})"_json, ctx);
    REQUIRE(res.is_ok());
    REQUIRE_FALSE(callback_called);  // fail-closed：未批准，不退出
    REQUIRE(res.value().text.find("not approved") != std::string::npos);
}

TEST_CASE("ExitPlanModeV2Tool does not downgrade bypass mode", "[tool][plan_mode]") {
    MockEventBus bus;
    auto_answer(bus, true);
    ToolContext ctx; fill_ctx(ctx, bus);
    ctx.permission_mode = PermissionMode::BypassPermissions;
    bool callback_called = false;
    ctx.on_permission_mode_changed = [&](PermissionMode) { callback_called = true; };

    ExitPlanModeV2Tool tool;
    auto res = tool.call(R"({"plan": "plan-text"})"_json, ctx);
    REQUIRE(res.is_ok());
    REQUIRE_FALSE(callback_called);  // Bypass 不降级
}
