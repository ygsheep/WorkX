/**
 * @file test_tool_call_tracker.cpp
 * @brief ToolCallTracker 单元测试
 * @details 覆盖 on_tool_call/on_tool_result 的嵌套层级管理、上下文存取、
 *          防御性边界（负数回滚）、reset_indent/reset 等场景
 */

#include <catch2/catch_test_macros.hpp>

#include "tui/model/tool_call_tracker.h"

using namespace tui;

// ============================================================================
// 初始状态
// ============================================================================

TEST_CASE("ToolCallTracker starts at zero indent and empty pending", "[tool_tracker][init]") {
    ToolCallTracker t;
    REQUIRE(t.indent_level() == 0);
    REQUIRE(t.pending_count() == 0);
}

// ============================================================================
// on_tool_call
// ============================================================================

TEST_CASE("ToolCallTracker on_tool_call returns previous indent and increments", "[tool_tracker][call]") {
    ToolCallTracker t;
    int prev = t.on_tool_call("call_1", "Read", R"({"file_path":"a.cpp"})");
    REQUIRE(prev == 0);
    REQUIRE(t.indent_level() == 1);
    REQUIRE(t.pending_count() == 1);
}

TEST_CASE("ToolCallTracker on_tool_call nests multiple levels", "[tool_tracker][call]") {
    ToolCallTracker t;
    t.on_tool_call("call_1", "Read", "{}");
    t.on_tool_call("call_2", "Write", "{}");
    t.on_tool_call("call_3", "Edit", "{}");
    REQUIRE(t.indent_level() == 3);
    REQUIRE(t.pending_count() == 3);
}

// ============================================================================
// on_tool_result
// ============================================================================

TEST_CASE("ToolCallTracker on_tool_result returns context and decrements", "[tool_tracker][result]") {
    ToolCallTracker t;
    t.on_tool_call("call_1", "Read", R"({"file_path":"a.cpp"})");
    t.on_tool_call("call_2", "Write", "{}");

    auto [info_opt, indent] = t.on_tool_result("call_2");
    REQUIRE(indent == 1);
    REQUIRE(info_opt.has_value());
    REQUIRE(info_opt->tool_name == "Write");
    REQUIRE(info_opt->arguments == "{}");
    REQUIRE(t.pending_count() == 1);
    REQUIRE(t.indent_level() == 1);
}

TEST_CASE("ToolCallTracker on_tool_result unknown call_id returns nullopt", "[tool_tracker][result]") {
    ToolCallTracker t;
    t.on_tool_call("call_1", "Read", "{}");

    auto [info_opt, indent] = t.on_tool_result("unknown_id");
    REQUIRE_FALSE(info_opt.has_value());
    REQUIRE(indent == 0);
    REQUIRE(t.pending_count() == 1);  // 原调用仍在
}

TEST_CASE("ToolCallTracker on_tool_result preserves LIFO order", "[tool_tracker][result]") {
    ToolCallTracker t;
    t.on_tool_call("call_1", "Read", "{}");
    t.on_tool_call("call_2", "Write", "{}");
    t.on_tool_call("call_3", "Edit", "{}");

    // 按完成顺序取出
    t.on_tool_result("call_3");
    t.on_tool_result("call_2");
    t.on_tool_result("call_1");
    REQUIRE(t.indent_level() == 0);
    REQUIRE(t.pending_count() == 0);
}

// ============================================================================
// 防御性：indent 不允许负数
// ============================================================================

TEST_CASE("ToolCallTracker on_tool_result defensive no negative indent", "[tool_tracker][defensive]") {
    ToolCallTracker t;
    // 没有任何 on_tool_call，直接 on_tool_result
    auto [info_opt, indent] = t.on_tool_result("unknown");
    REQUIRE(indent == 0);
    REQUIRE(t.indent_level() == 0);
}

TEST_CASE("ToolCallTracker on_tool_result clamps to zero after over-subtract", "[tool_tracker][defensive]") {
    ToolCallTracker t;
    t.on_tool_call("call_1", "Read", "{}");
    t.on_tool_result("call_1");
    REQUIRE(t.indent_level() == 0);

    // 再 on_tool_result，indent 不应变负
    t.on_tool_result("call_2");
    REQUIRE(t.indent_level() == 0);
}

// ============================================================================
// reset_indent
// ============================================================================

TEST_CASE("ToolCallTracker reset_indent only resets indent not pending", "[tool_tracker][reset_indent]") {
    ToolCallTracker t;
    t.on_tool_call("call_1", "Read", "{}");
    t.on_tool_call("call_2", "Write", "{}");

    t.reset_indent();
    REQUIRE(t.indent_level() == 0);
    REQUIRE(t.pending_count() == 2);  // pending 不清除
}

// ============================================================================
// reset
// ============================================================================

TEST_CASE("ToolCallTracker reset clears all", "[tool_tracker][reset]") {
    ToolCallTracker t;
    t.on_tool_call("call_1", "Read", "{}");
    t.on_tool_call("call_2", "Write", "{}");

    t.reset();
    REQUIRE(t.indent_level() == 0);
    REQUIRE(t.pending_count() == 0);
}

// ============================================================================
// 综合场景：模拟 ReAct 多步工具调用
// ============================================================================

TEST_CASE("ToolCallTracker simulates ReAct multi-step flow", "[tool_tracker][integration]") {
    ToolCallTracker t;

    // Step 1: Read 文件
    int indent0 = t.on_tool_call("c1", "Read", R"({"file_path":"main.cpp"})");
    REQUIRE(indent0 == 0);
    auto [info1, indent1] = t.on_tool_result("c1");
    REQUIRE(info1.has_value());
    REQUIRE(info1->tool_name == "Read");
    REQUIRE(indent1 == 0);

    // Step 2: Edit 文件
    int indent2 = t.on_tool_call("c2", "Edit", R"({"file_path":"main.cpp"})");
    REQUIRE(indent2 == 0);
    auto [info2, indent3] = t.on_tool_result("c2");
    REQUIRE(info2->tool_name == "Edit");
    REQUIRE(indent3 == 0);

    // AgentDone 重置
    t.reset_indent();
    REQUIRE(t.indent_level() == 0);
    REQUIRE(t.pending_count() == 0);
}
