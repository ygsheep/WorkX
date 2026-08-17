/**
 * @file test_view_model.cpp
 * @brief ViewModel 单元测试（ftxtui 无头逻辑）
 * @details 覆盖全部 15 个 Action 的 apply 语义：流式追加、滚动封口、工具块、
 *          错误/忙碌/权限/退出/提示，以及 active_stream 的复用/新建语义。
 */

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

#include "bridge/action.h"
#include "vm/view_model.h"

using namespace ftxtui;

// ============================================================================
// active_stream / has_active_stream
// ============================================================================

TEST_CASE("ViewModel active_stream creates a new assistant on empty", "[view_model][stream]") {
    ViewModel vm;
    REQUIRE_FALSE(vm.has_active_stream());
    auto& m = vm.active_stream();
    REQUIRE(m.role == MsgRole::Assistant);
    REQUIRE(vm.has_active_stream());
    REQUIRE(vm.messages.size() == 1);
}

TEST_CASE("ViewModel active_stream reuses an unsealed assistant", "[view_model][stream]") {
    ViewModel vm;
    auto& m1 = vm.active_stream();
    m1.text = "abc";
    auto& m2 = vm.active_stream();
    REQUIRE(&m1 == &m2);           // 复用同一节点
    REQUIRE(vm.messages.size() == 1);
}

TEST_CASE("ViewModel active_stream creates a new node after sealing", "[view_model][stream]") {
    ViewModel vm;
    REQUIRE(vm.apply(ActionTurnDone{.full_content = "done"}));  // 封口
    REQUIRE_FALSE(vm.has_active_stream());
    auto& next = vm.active_stream();
    REQUIRE(next.text.empty());
    REQUIRE(vm.messages.size() == 2);
}

TEST_CASE("ViewModel has_active_stream false after error message", "[view_model][stream]") {
    ViewModel vm;
    vm.apply(ActionError{.message = "boom"});
    REQUIRE_FALSE(vm.has_active_stream());
    REQUIRE(vm.messages.back().role == MsgRole::Error);
}

// ============================================================================
// ActionAppendMessage
// ============================================================================

TEST_CASE("ViewModel append user message marks user + sealed", "[view_model][append]") {
    ViewModel vm;
    REQUIRE(vm.apply(ActionAppendMessage{.role = "user", .text = "hi"}));
    REQUIRE(vm.messages.size() == 1);
    REQUIRE(vm.messages[0].role == MsgRole::User);
    REQUIRE(vm.messages[0].sealed);
    REQUIRE(vm.messages[0].text == "hi");
}

TEST_CASE("ViewModel append assistant message marks assistant + sealed", "[view_model][append]") {
    ViewModel vm;
    vm.apply(ActionAppendMessage{.role = "assistant", .text = "reply"});
    REQUIRE(vm.messages[0].role == MsgRole::Assistant);
    REQUIRE(vm.messages[0].sealed);
}

// ============================================================================
// 流式增量
// ============================================================================

TEST_CASE("ViewModel token delta appends and flips streaming", "[view_model][token]") {
    ViewModel vm;
    REQUIRE(vm.apply(ActionTokenDelta{.content_delta = "Hello "}));
    REQUIRE(vm.apply(ActionTokenDelta{.content_delta = "world"}));
    REQUIRE(vm.active_stream().text == "Hello world");
    REQUIRE(vm.active_stream().streaming);
}

TEST_CASE("ViewModel reasoning delta marks reasoned and expands by default", "[view_model][reasoning]") {
    ViewModel vm;
    vm.apply(ActionReasoningDelta{.delta = "think"});
    auto& m = vm.messages.back();
    REQUIRE(m.reasoned);
    REQUIRE(m.reasoning_expanded == vm.card_defaults.reasoning_expanded);
    REQUIRE(m.reasoning == "think");
}

TEST_CASE("ViewModel reasoning empty delta does not mark reasoned", "[view_model][reasoning]") {
    ViewModel vm;
    vm.apply(ActionReasoningDelta{.delta = ""});
    REQUIRE_FALSE(vm.messages.back().reasoned);
}

TEST_CASE("ViewModel step done clears streaming on back assistant", "[view_model][step_done]") {
    ViewModel vm;
    vm.apply(ActionTokenDelta{.content_delta = "x"});
    REQUIRE(vm.active_stream().streaming);
    vm.apply(ActionStepDone{});
    REQUIRE_FALSE(vm.active_stream().streaming);
}

// ============================================================================
// ActionTurnDone
// ============================================================================

TEST_CASE("ViewModel turn done fills text when empty and seals", "[view_model][turn_done]") {
    ViewModel vm;
    vm.apply(ActionTurnDone{.full_content = "final", .prompt_tokens = 10,
                            .generated_tokens = 20, .cache_read_input_tokens = 5,
                            .prompt_ms = 100.0, .generation_ms = 200.0});
    auto& m = vm.messages.back();
    REQUIRE(m.text == "final");
    REQUIRE(m.sealed);
    REQUIRE_FALSE(m.streaming);
    REQUIRE(m.prompt_tokens == 10);
    REQUIRE(m.generated_tokens == 20);
    REQUIRE(m.cache_read_tokens == 5);
    REQUIRE(m.duration_ms == 300.0);  // 100+200 精确可表示
    REQUIRE(m.reasoning_ms == 100.0);
    REQUIRE(vm.busy == false);
    REQUIRE(vm.total_tokens == 35);
    REQUIRE(vm.sidebar.context_used == 10);
}

TEST_CASE("ViewModel turn done keeps existing streamed text", "[view_model][turn_done]") {
    ViewModel vm;
    vm.apply(ActionTokenDelta{.content_delta = "streamed"});
    vm.apply(ActionTurnDone{.full_content = "final"});
    REQUIRE(vm.messages.back().text == "streamed");
}

// ============================================================================
// 工具调用
// ============================================================================

TEST_CASE("ViewModel begin tool creates running block", "[view_model][tool]") {
    ViewModel vm;
    REQUIRE(vm.apply(ActionBeginTool{.tool_name = "Read", .call_id = "c1",
                                     .arguments = "{}"}));
    auto& t = vm.messages.back().tool_calls[0];
    REQUIRE(t.tool_name == "Read");
    REQUIRE(t.call_id == "c1");
    REQUIRE(t.running);
    REQUIRE(t.expanded == vm.card_defaults.tool_expanded);
    REQUIRE(vm.messages.back().tool_use_ids.size() == 1);
}

TEST_CASE("ViewModel begin tool deduplicates same call_id", "[view_model][tool]") {
    ViewModel vm;
    vm.apply(ActionBeginTool{.tool_name = "Read", .call_id = "c1"});
    REQUIRE_FALSE(vm.apply(ActionBeginTool{.tool_name = "Read", .call_id = "c1"}));  // 重复
    REQUIRE(vm.messages.back().tool_calls.size() == 1);
}

TEST_CASE("ViewModel end tool marks done and records result", "[view_model][tool]") {
    ViewModel vm;
    vm.apply(ActionBeginTool{.tool_name = "Read", .call_id = "c1"});
    REQUIRE(vm.apply(ActionEndTool{.call_id = "c1", .result = "content"}));
    auto& t = vm.messages.back().tool_calls[0];
    REQUIRE(t.done);
    REQUIRE_FALSE(t.running);
    REQUIRE(t.result == "content");
    REQUIRE_FALSE(t.is_error);
    REQUIRE_FALSE(t.expanded);  // 成功默认收起
}

TEST_CASE("ViewModel end tool error expands by default", "[view_model][tool]") {
    ViewModel vm;
    vm.apply(ActionBeginTool{.tool_name = "Bash", .call_id = "c1"});
    vm.apply(ActionEndTool{.call_id = "c1", .result = "err", .is_error = true});
    auto& t = vm.messages.back().tool_calls[0];
    REQUIRE(t.is_error);
    REQUIRE(t.expanded);
}

TEST_CASE("ViewModel end tool unknown call_id returns false", "[view_model][tool]") {
    ViewModel vm;
    REQUIRE_FALSE(vm.apply(ActionEndTool{.call_id = "missing", .result = ""}));
}

// ============================================================================
// ActionAgentDone
// ============================================================================

TEST_CASE("ViewModel agent done seals unsealed assistant and fills text", "[view_model][agent_done]") {
    ViewModel vm;
    vm.apply(ActionSetBusy{.busy = true});  // 预留 assistant
    vm.apply(ActionAgentDone{.final_answer = "answer", .total_duration_ms = 99.0});
    auto& m = vm.messages.back();
    REQUIRE(m.sealed);
    REQUIRE(m.text == "answer");
    REQUIRE(m.duration_ms == 99.0);
    REQUIRE_FALSE(vm.busy);
}

TEST_CASE("ViewModel agent done creates fallback assistant without active stream", "[view_model][agent_done]") {
    ViewModel vm;
    // 无流式节点、无未封口 assistant
    vm.apply(ActionAppendMessage{.role = "user", .text = "q"});
    vm.apply(ActionAgentDone{.final_answer = "answer"});
    REQUIRE(vm.messages.back().role == MsgRole::Assistant);
    REQUIRE(vm.messages.back().text == "answer");
    REQUIRE(vm.messages.back().sealed);
}

TEST_CASE("ViewModel agent done with empty answer does nothing", "[view_model][agent_done]") {
    ViewModel vm;
    vm.apply(ActionAgentDone{});
    REQUIRE(vm.messages.empty());
}

// ============================================================================
// ActionSetBusy
// ============================================================================

TEST_CASE("ViewModel set busy reserves an assistant node", "[view_model][busy]") {
    ViewModel vm;
    REQUIRE(vm.apply(ActionSetBusy{.busy = true}));
    REQUIRE(vm.busy);
    REQUIRE_FALSE(vm.messages.empty());
    REQUIRE_FALSE(vm.messages.back().streaming);
    vm.apply(ActionSetBusy{.busy = false});
    REQUIRE_FALSE(vm.busy);
}

// ============================================================================
// 杂项
// ============================================================================

TEST_CASE("ViewModel permissions updates and deduplicates", "[view_model][misc]") {
    ViewModel vm;
    REQUIRE(vm.apply(ActionPermissions{.label = "plan"}));
    REQUIRE(vm.sidebar.permission == "plan");
    REQUIRE_FALSE(vm.apply(ActionPermissions{.label = "plan"}));  // 相同不变化
    REQUIRE(vm.apply(ActionPermissions{.label = "bypass"}));
    REQUIRE(vm.sidebar.permission == "bypass");
}

TEST_CASE("ViewModel shutdown raises pending_exit", "[view_model][misc]") {
    ViewModel vm;
    REQUIRE(vm.apply(ActionShutdown{}));
    REQUIRE(vm.pending_exit);
}

TEST_CASE("ViewModel toast stores prompt echo", "[view_model][misc]") {
    ViewModel vm;
    REQUIRE(vm.apply(ActionToast{.text = "command output"}));
    REQUIRE(vm.prompt_echo == "command output");
}

TEST_CASE("ViewModel models loaded is ignored by view model", "[view_model][misc]") {
    ViewModel vm;
    REQUIRE_FALSE(vm.apply(ActionModelsLoaded{.models = {"m1"}}));
}

// ============================================================================
// 综合：消息历史组装
// ============================================================================

TEST_CASE("ViewModel assembles a full multi-turn history", "[view_model][integration]") {
    ViewModel vm;
    vm.apply(ActionAppendMessage{.role = "user", .text = "Q1"});
    vm.apply(ActionSetBusy{.busy = true});
    vm.apply(ActionReasoningDelta{.delta = "reason"});
    vm.apply(ActionTokenDelta{.content_delta = "A1"});
    vm.apply(ActionAgentDone{.final_answer = "A1", .total_tool_calls = 0});
    vm.apply(ActionAppendMessage{.role = "user", .text = "Q2"});

    REQUIRE(vm.messages.size() == 3);
    REQUIRE(vm.messages[0].role == MsgRole::User);
    REQUIRE(vm.messages[0].text == "Q1");
    REQUIRE(vm.messages[1].role == MsgRole::Assistant);
    REQUIRE(vm.messages[1].reasoned);
    REQUIRE(vm.messages[1].text == "A1");
    REQUIRE(vm.messages[1].sealed);
    REQUIRE(vm.messages[2].role == MsgRole::User);
    REQUIRE(vm.messages[2].text == "Q2");
}