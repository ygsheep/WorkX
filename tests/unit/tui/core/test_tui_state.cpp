/**
 * @file test_tui_state.cpp
 * @brief TuiState / TuiStateMachine 单元测试
 * @details 覆盖状态枚举名称、合法转换、非法转换、force_state、DETAIL_VIEW 叠加态
 *          以及典型 ReAct / 错误恢复 / 中断流程
 */

#include <catch2/catch_test_macros.hpp>

#include "tui/core/tui_state.h"

using namespace agent;

// ============================================================================
// tui_state_name
// ============================================================================

TEST_CASE("tui_state_name returns correct names", "[tui_state][name]") {
    REQUIRE(tui_state_name(TuiState::IDLE) == "idle");
    REQUIRE(tui_state_name(TuiState::THINKING) == "thinking");
    REQUIRE(tui_state_name(TuiState::STREAMING) == "streaming");
    REQUIRE(tui_state_name(TuiState::TOOL_RUNNING) == "tool");
    REQUIRE(tui_state_name(TuiState::ERROR) == "error");
    REQUIRE(tui_state_name(TuiState::DETAIL_VIEW) == "detail");
}

TEST_CASE("tui_state_name default returns unknown", "[tui_state][name]") {
    auto unknown = static_cast<TuiState>(999);
    REQUIRE(tui_state_name(unknown) == "unknown");
}

// ============================================================================
// TuiStateMachine 初始状态
// ============================================================================

TEST_CASE("TuiStateMachine starts at IDLE", "[tui_state][init]") {
    TuiStateMachine sm;
    REQUIRE(sm.current() == TuiState::IDLE);
}

// ============================================================================
// is_valid_transition — 同状态
// ============================================================================

TEST_CASE("is_valid_transition same state is valid", "[tui_state][transition]") {
    REQUIRE(TuiStateMachine::is_valid_transition(TuiState::IDLE, TuiState::IDLE));
    REQUIRE(TuiStateMachine::is_valid_transition(TuiState::STREAMING, TuiState::STREAMING));
    REQUIRE(TuiStateMachine::is_valid_transition(TuiState::ERROR, TuiState::ERROR));
}

// ============================================================================
// is_valid_transition — IDLE
// ============================================================================

TEST_CASE("is_valid_transition IDLE only allows THINKING", "[tui_state][transition][idle]") {
    REQUIRE(TuiStateMachine::is_valid_transition(TuiState::IDLE, TuiState::THINKING));
    REQUIRE_FALSE(TuiStateMachine::is_valid_transition(TuiState::IDLE, TuiState::STREAMING));
    REQUIRE_FALSE(TuiStateMachine::is_valid_transition(TuiState::IDLE, TuiState::TOOL_RUNNING));
    REQUIRE_FALSE(TuiStateMachine::is_valid_transition(TuiState::IDLE, TuiState::ERROR));
}

// ============================================================================
// is_valid_transition — THINKING
// ============================================================================

TEST_CASE("is_valid_transition THINKING allows STREAMING/TOOL_RUNNING/ERROR/IDLE", "[tui_state][transition][thinking]") {
    REQUIRE(TuiStateMachine::is_valid_transition(TuiState::THINKING, TuiState::STREAMING));
    REQUIRE(TuiStateMachine::is_valid_transition(TuiState::THINKING, TuiState::TOOL_RUNNING));
    REQUIRE(TuiStateMachine::is_valid_transition(TuiState::THINKING, TuiState::ERROR));
    REQUIRE(TuiStateMachine::is_valid_transition(TuiState::THINKING, TuiState::IDLE));
}

// ============================================================================
// is_valid_transition — STREAMING
// ============================================================================

TEST_CASE("is_valid_transition STREAMING allows IDLE/TOOL_RUNNING/THINKING/ERROR", "[tui_state][transition][streaming]") {
    REQUIRE(TuiStateMachine::is_valid_transition(TuiState::STREAMING, TuiState::IDLE));
    REQUIRE(TuiStateMachine::is_valid_transition(TuiState::STREAMING, TuiState::TOOL_RUNNING));
    REQUIRE(TuiStateMachine::is_valid_transition(TuiState::STREAMING, TuiState::THINKING));
    REQUIRE(TuiStateMachine::is_valid_transition(TuiState::STREAMING, TuiState::ERROR));
}

// ============================================================================
// is_valid_transition — TOOL_RUNNING
// ============================================================================

TEST_CASE("is_valid_transition TOOL_RUNNING allows STREAMING/IDLE/ERROR", "[tui_state][transition][tool_running]") {
    REQUIRE(TuiStateMachine::is_valid_transition(TuiState::TOOL_RUNNING, TuiState::STREAMING));
    REQUIRE(TuiStateMachine::is_valid_transition(TuiState::TOOL_RUNNING, TuiState::IDLE));
    REQUIRE(TuiStateMachine::is_valid_transition(TuiState::TOOL_RUNNING, TuiState::ERROR));
    REQUIRE_FALSE(TuiStateMachine::is_valid_transition(TuiState::TOOL_RUNNING, TuiState::THINKING));
}

// ============================================================================
// is_valid_transition — ERROR
// ============================================================================

TEST_CASE("is_valid_transition ERROR allows IDLE/THINKING", "[tui_state][transition][error]") {
    REQUIRE(TuiStateMachine::is_valid_transition(TuiState::ERROR, TuiState::IDLE));
    REQUIRE(TuiStateMachine::is_valid_transition(TuiState::ERROR, TuiState::THINKING));
    REQUIRE_FALSE(TuiStateMachine::is_valid_transition(TuiState::ERROR, TuiState::STREAMING));
    REQUIRE_FALSE(TuiStateMachine::is_valid_transition(TuiState::ERROR, TuiState::TOOL_RUNNING));
}

// ============================================================================
// DETAIL_VIEW 叠加态
// ============================================================================

TEST_CASE("DETAIL_VIEW can be entered from any state", "[tui_state][detail_view]") {
    for (auto from : {TuiState::IDLE, TuiState::THINKING, TuiState::STREAMING,
                      TuiState::TOOL_RUNNING, TuiState::ERROR}) {
        REQUIRE(TuiStateMachine::is_valid_transition(from, TuiState::DETAIL_VIEW));
    }
}

TEST_CASE("DETAIL_VIEW can return to any state", "[tui_state][detail_view]") {
    for (auto to : {TuiState::IDLE, TuiState::THINKING, TuiState::STREAMING,
                    TuiState::TOOL_RUNNING, TuiState::ERROR}) {
        REQUIRE(TuiStateMachine::is_valid_transition(TuiState::DETAIL_VIEW, to));
    }
}

// ============================================================================
// transition_to
// ============================================================================

TEST_CASE("transition_to valid transition updates state", "[tui_state][transition_to]") {
    TuiStateMachine sm;
    REQUIRE(sm.transition_to(TuiState::THINKING));
    REQUIRE(sm.current() == TuiState::THINKING);

    REQUIRE(sm.transition_to(TuiState::STREAMING));
    REQUIRE(sm.current() == TuiState::STREAMING);

    REQUIRE(sm.transition_to(TuiState::IDLE));
    REQUIRE(sm.current() == TuiState::IDLE);
}

TEST_CASE("transition_to invalid transition does not update", "[tui_state][transition_to]") {
    TuiStateMachine sm;
    REQUIRE_FALSE(sm.transition_to(TuiState::STREAMING));
    REQUIRE(sm.current() == TuiState::IDLE);

    REQUIRE_FALSE(sm.transition_to(TuiState::ERROR));
    REQUIRE(sm.current() == TuiState::IDLE);
}

TEST_CASE("transition_to same state is no-op but returns true", "[tui_state][transition_to]") {
    TuiStateMachine sm;
    REQUIRE(sm.transition_to(TuiState::IDLE));
    REQUIRE(sm.current() == TuiState::IDLE);
}

// ============================================================================
// force_state
// ============================================================================

TEST_CASE("force_state bypasses validation", "[tui_state][force_state]") {
    TuiStateMachine sm;
    sm.force_state(TuiState::STREAMING);
    REQUIRE(sm.current() == TuiState::STREAMING);

    sm.force_state(TuiState::ERROR);
    REQUIRE(sm.current() == TuiState::ERROR);
}

TEST_CASE("force_state then transition_to resumes normal validation", "[tui_state][force_state]") {
    TuiStateMachine sm;
    sm.force_state(TuiState::STREAMING);
    // 从 STREAMING 出发的合法转换应正常工作
    REQUIRE(sm.transition_to(TuiState::IDLE));
    REQUIRE(sm.current() == TuiState::IDLE);
}

// ============================================================================
// 典型流程
// ============================================================================

TEST_CASE("TuiStateMachine typical ReAct flow", "[tui_state][flow]") {
    TuiStateMachine sm;
    // IDLE -> THINKING -> STREAMING -> TOOL_RUNNING -> STREAMING -> IDLE
    REQUIRE(sm.transition_to(TuiState::THINKING));
    REQUIRE(sm.transition_to(TuiState::STREAMING));
    REQUIRE(sm.transition_to(TuiState::TOOL_RUNNING));
    REQUIRE(sm.transition_to(TuiState::STREAMING));
    REQUIRE(sm.transition_to(TuiState::IDLE));
    REQUIRE(sm.current() == TuiState::IDLE);
}

TEST_CASE("TuiStateMachine error recovery flow", "[tui_state][flow]") {
    TuiStateMachine sm;
    REQUIRE(sm.transition_to(TuiState::THINKING));
    REQUIRE(sm.transition_to(TuiState::ERROR));
    REQUIRE(sm.transition_to(TuiState::THINKING));
    REQUIRE(sm.current() == TuiState::THINKING);
}

TEST_CASE("TuiStateMachine interrupt flow", "[tui_state][flow]") {
    TuiStateMachine sm;
    REQUIRE(sm.transition_to(TuiState::THINKING));
    REQUIRE(sm.transition_to(TuiState::STREAMING));
    REQUIRE(sm.transition_to(TuiState::IDLE));
    REQUIRE(sm.current() == TuiState::IDLE);
}

TEST_CASE("TuiStateMachine DETAIL_VIEW overlay flow", "[tui_state][flow]") {
    TuiStateMachine sm;
    REQUIRE(sm.transition_to(TuiState::THINKING));
    REQUIRE(sm.transition_to(TuiState::STREAMING));
    // 进入详情视图（叠加态）
    REQUIRE(sm.transition_to(TuiState::DETAIL_VIEW));
    REQUIRE(sm.current() == TuiState::DETAIL_VIEW);
    // 退出回 STREAMING
    REQUIRE(sm.transition_to(TuiState::STREAMING));
    REQUIRE(sm.current() == TuiState::STREAMING);
}
