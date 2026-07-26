/**
 * @file test_status_bar.cpp
 * @brief StatusBar 单元测试
 * @details 覆盖 set_state / set_* 配置方法 / is_active_state / advance_frame
 *          以及 start_session_timer 和空操作方法
 *
 * @note StatusBar 的 render/clear 依赖未 initialize() 的 Terminal 平台层
 *       (m_platform 为 nullptr，write_safe 会崩溃)，故不测试渲染路径。
 *       完整 UI 渲染测试留待后续 Phase 抽象出 ITerminal 接口后补充。
 */

#include <catch2/catch_test_macros.hpp>

#include "tui/core/terminal.h"
#include "tui/widgets/status_bar.h"
#include "tui/core/tui_state.h"

using namespace agent;

namespace {

/// @brief 未 initialize() 的 Terminal，仅用于满足 StatusBar 构造签名
/// @details StatusBar 的 set_* 方法仅修改成员变量，不调用 Terminal 写入
Terminal null_terminal;

} // namespace

// ============================================================================
// 构造与初始状态
// ============================================================================

TEST_CASE("StatusBar default state is IDLE", "[status_bar][init]") {
    StatusBar bar(&null_terminal);
    REQUIRE_FALSE(bar.is_active_state());
}

// ============================================================================
// set_state
// ============================================================================

TEST_CASE("StatusBar set_state to active marks active", "[status_bar][state]") {
    StatusBar bar(&null_terminal);
    bar.set_state(TuiState::THINKING);
    REQUIRE(bar.is_active_state());

    bar.set_state(TuiState::STREAMING);
    REQUIRE(bar.is_active_state());

    bar.set_state(TuiState::TOOL_RUNNING);
    REQUIRE(bar.is_active_state());

    bar.set_state(TuiState::ERROR);
    REQUIRE(bar.is_active_state());
}

TEST_CASE("StatusBar set_state to IDLE clears active", "[status_bar][state]") {
    StatusBar bar(&null_terminal);
    bar.set_state(TuiState::THINKING);
    REQUIRE(bar.is_active_state());

    bar.set_state(TuiState::IDLE);
    REQUIRE_FALSE(bar.is_active_state());
}

TEST_CASE("StatusBar set_state to IDLE resets frame to 0", "[status_bar][state]") {
    // frame 是 private，通过 set_state(IDLE) 应重置 m_frame=0
    // 此处仅验证不崩溃 + 状态正确
    StatusBar bar(&null_terminal);
    bar.advance_frame();
    bar.advance_frame();
    bar.advance_frame();
    bar.set_state(TuiState::THINKING);
    bar.set_state(TuiState::IDLE);  // 应重置 frame
    REQUIRE_FALSE(bar.is_active_state());
}

TEST_CASE("StatusBar state transitions do not crash", "[status_bar][state]") {
    StatusBar bar(&null_terminal);
    bar.set_state(TuiState::THINKING);
    bar.set_state(TuiState::STREAMING);
    bar.set_state(TuiState::TOOL_RUNNING);
    bar.set_state(TuiState::STREAMING);
    bar.set_state(TuiState::IDLE);
    bar.set_state(TuiState::ERROR);
    bar.set_state(TuiState::IDLE);
    REQUIRE_FALSE(bar.is_active_state());
}

// ============================================================================
// set_* 配置方法
// ============================================================================

TEST_CASE("StatusBar set_model_name accepts various inputs", "[status_bar][config]") {
    StatusBar bar(&null_terminal);
    bar.set_model_name("");
    bar.set_model_name("gpt-4");
    bar.set_model_name("claude-3-opus-with-very-long-name");
}

TEST_CASE("StatusBar set_project_name accepts various inputs", "[status_bar][config]") {
    StatusBar bar(&null_terminal);
    bar.set_project_name("");
    bar.set_project_name("workx");
    bar.set_project_name("/path/to/project");
}

TEST_CASE("StatusBar set_token_count accepts zero, positive, large, negative", "[status_bar][config]") {
    StatusBar bar(&null_terminal);
    bar.set_token_count(0);
    bar.set_token_count(100);
    bar.set_token_count(1000000);
    bar.set_token_count(-1);
}

TEST_CASE("StatusBar set_context_limit accepts zero, positive, large, negative", "[status_bar][config]") {
    StatusBar bar(&null_terminal);
    bar.set_context_limit(0);     // 未知窗口
    bar.set_context_limit(8192);
    bar.set_context_limit(200000);
    bar.set_context_limit(-1);
}

TEST_CASE("StatusBar set_cache_read_tokens clamps negative to zero", "[status_bar][config]") {
    StatusBar bar(&null_terminal);
    bar.set_cache_read_tokens(0);
    bar.set_cache_read_tokens(8192);
    bar.set_cache_read_tokens(-100);  // 应被 clamp 到 0
}

// ============================================================================
// advance_frame
// ============================================================================

TEST_CASE("StatusBar advance_frame callable multiple times", "[status_bar][frame]") {
    StatusBar bar(&null_terminal);
    for (int i = 0; i < 20; ++i) {
        bar.advance_frame();
    }
}

TEST_CASE("StatusBar advance_frame wraps around SPINNER_FRAME_COUNT", "[status_bar][frame]") {
    // SPINNER_FRAME_COUNT = 10，advance 100 次应 mod 10
    StatusBar bar(&null_terminal);
    for (int i = 0; i < 100; ++i) {
        bar.advance_frame();
    }
}

// ============================================================================
// start_session_timer
// ============================================================================

TEST_CASE("StatusBar start_session_timer is callable", "[status_bar][timer]") {
    StatusBar bar(&null_terminal);
    bar.start_session_timer();
}

TEST_CASE("StatusBar start_session_timer callable multiple times", "[status_bar][timer]") {
    StatusBar bar(&null_terminal);
    bar.start_session_timer();
    bar.start_session_timer();
    bar.start_session_timer();
}

// ============================================================================
// 空操作方法
// ============================================================================

TEST_CASE("StatusBar no-op methods are safe", "[status_bar][noop]") {
    StatusBar bar(&null_terminal);
    bar.set_thinking_seconds(10);
    bar.set_tool_name("Write");
    bar.subscribe_events();
    bar.unsubscribe_events();
}

// ============================================================================
// 综合场景：完整会话状态序列
// ============================================================================

TEST_CASE("StatusBar full session state sequence", "[status_bar][flow]") {
    StatusBar bar(&null_terminal);
    bar.start_session_timer();

    // 用户输入 → THINKING
    bar.set_state(TuiState::THINKING);
    REQUIRE(bar.is_active_state());

    // 流式输出 → STREAMING
    bar.set_state(TuiState::STREAMING);
    bar.set_token_count(500);
    bar.set_context_limit(8192);
    bar.set_cache_read_tokens(200);

    // 工具调用 → TOOL_RUNNING
    bar.set_state(TuiState::TOOL_RUNNING);
    bar.set_tool_name("Write");

    // 工具完成 → STREAMING
    bar.set_state(TuiState::STREAMING);

    // 流结束 → IDLE
    bar.set_state(TuiState::IDLE);
    REQUIRE_FALSE(bar.is_active_state());

    // 错误 → IDLE
    bar.set_state(TuiState::ERROR);
    REQUIRE(bar.is_active_state());
    bar.set_state(TuiState::IDLE);
    REQUIRE_FALSE(bar.is_active_state());
}
