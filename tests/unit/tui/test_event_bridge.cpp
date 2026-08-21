/**
 * @file test_event_bridge.cpp
 * @brief EventBridge 单元测试（B4：stop() 真正按 token 退订）
 * @details 用 MockEventBus 验证：start() 订阅全部事件 → stop() 逐个精确退订
 *          （订阅者清零、unsubscribe 被调用），不误伤其他订阅者。
 */

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "bridge/action_queue.h"
#include "bridge/event_bridge.h"
#include "core/events/agent_events.h"
#include "core/events/stream_events.h"
#include "core/events/system_events.h"
#include "helpers/mock_event_bus.h"

using namespace ftxtui;
using namespace agent::test;

namespace {

/// @brief 订阅后统计 EventBridge 管理的订阅类型数量
size_t bridge_subscriber_count(MockEventBus& bus) {
    size_t n = 0;
    n += bus.subscriber_count_typed<agent::StreamTokenEvent>();
    n += bus.subscriber_count_typed<agent::StreamDoneEvent>();
    n += bus.subscriber_count_typed<agent::StepDoneEvent>();
    n += bus.subscriber_count_typed<agent::StreamErrorEvent>();
    n += bus.subscriber_count_typed<agent::ToolCallEvent>();
    n += bus.subscriber_count_typed<agent::ToolResultEvent>();
    n += bus.subscriber_count_typed<agent::AgentDoneEvent>();
    n += bus.subscriber_count_typed<agent::AskUserRequestEvent>();
    n += bus.subscriber_count_typed<agent::AskUserTimeoutEvent>();
    n += bus.subscriber_count_typed<agent::EnterPlanModeEvent>();
    n += bus.subscriber_count_typed<agent::ExitPlanModeEvent>();
    n += bus.subscriber_count_typed<agent::CacheDiagnosticsEvent>();
    n += bus.subscriber_count_typed<agent::CompactionPausedEvent>();
    n += bus.subscriber_count_typed<agent::SubAgentProgressEvent>();
    n += bus.subscriber_count_typed<agent::SubAgentCompletedEvent>();
    n += bus.subscriber_count_typed<agent::TodoUpdatedEvent>();  // #24：待办清单更新
    n += bus.subscriber_count_typed<agent::ShutdownEvent>();
    return n;
}

}  // namespace

TEST_CASE("EventBridge start subscribes all UI events", "[event_bridge][start]") {
    MockEventBus bus;
    ActionQueue queue;
    EventBridge bridge(bus, queue);
    bridge.start();

    REQUIRE(bridge_subscriber_count(bus) == 17);
    REQUIRE(bus.total_subscriber_count() == 17);
}

TEST_CASE("EventBridge stop unsubscribes every registered event", "[event_bridge][stop]") {
    MockEventBus bus;
    ActionQueue queue;
    EventBridge bridge(bus, queue);
    bridge.start();

    bridge.stop();
    REQUIRE(bridge_subscriber_count(bus) == 0);
    REQUIRE(bus.total_subscriber_count() == 0);
}

TEST_CASE("EventBridge stop does not affect unrelated subscribers", "[event_bridge][stop]") {
    MockEventBus bus;
    ActionQueue queue;

    // 一个与本桥无关的订阅者（模拟 EventBus 上其他组件的订阅）
    auto unrelated_token = bus.subscribe<agent::StreamTokenEvent>(
        [](const agent::StreamTokenEvent&) {});

    EventBridge bridge(bus, queue);
    bridge.start();
    bridge.stop();

    // 本桥退订不影响无关订阅者
    REQUIRE(bus.subscriber_count_typed<agent::StreamTokenEvent>() == 1);
    REQUIRE(bus.total_subscriber_count() == 1);
    bus.unsubscribe<agent::StreamTokenEvent>(unrelated_token);
}

TEST_CASE("EventBridge stop is idempotent", "[event_bridge][stop]") {
    MockEventBus bus;
    ActionQueue queue;
    EventBridge bridge(bus, queue);
    bridge.start();
    bridge.stop();
    // 二次 stop 安全（无异常、不重复退订造成计数错乱）
    REQUIRE_NOTHROW(bridge.stop());
    REQUIRE(bus.total_subscriber_count() == 0);
}

TEST_CASE("EventBridge dispatch maps events to actions", "[event_bridge][dispatch]") {
    MockEventBus bus;
    ActionQueue queue;
    EventBridge bridge(bus, queue);
    bridge.start();
    bus.set_dispatch_enabled(true);

    // 派发一个 StreamTokenEvent，应入队 TokenDelta action
    bus.publish(agent::StreamTokenEvent{
        .session_id = "s",
        .content_delta = "hi",
        .reasoning_delta = "",
        .is_thinking = false,
        .token_count = 1,
    });
    auto actions = queue.drain();
    REQUIRE(actions.size() == 1);
    const auto* delta = std::get_if<ActionTokenDelta>(&actions[0]);
    REQUIRE(delta != nullptr);
    REQUIRE(delta->content_delta == "hi");

    // 停用派发后 stop：不再入队
    bridge.stop();
    queue.drain();
    bus.publish(agent::StreamTokenEvent{
        .session_id = "s", .content_delta = "bye", .reasoning_delta = "",
        .is_thinking = false, .token_count = 1,
    });
    REQUIRE(queue.empty());
}