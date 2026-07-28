/**
 * @file test_terminal_overlay.cpp
 * @brief Terminal overlay 机制单元测试
 * @details 覆盖 is_overlay_active() 状态查询、begin/end_overlay 安全性、
 *          以及 DisplayBuffer 快照/恢复（overlay 机制的基础组件）
 *
 * @note Terminal 的 begin/end_overlay 依赖 m_display_buffer（由 initialize() 创建），
 *       未 initialize() 时 m_display_buffer 为 nullptr，begin_overlay 会安全跳过。
 *       完整 UI 渲染测试留待后续 Phase 抽象出 ITerminal 接口后补充。
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "tui/core/terminal.h"
#include "tui/core/display_buffer.h"
#include "core/events/event_bus.h"
#include "core/config/config_manager.h"
#include "core/task/task_manager.h"

using namespace tui;

namespace {

/// @brief 未 initialize() 的 Terminal，用于测试状态查询和安全性
Terminal null_terminal(&agent::EventBus::instance(),
                       &agent::ConfigManager::instance(),
                       &agent::TaskManager::instance(),
                       TerminalConfig{});

} // namespace

// ============================================================================
// is_overlay_active() 状态查询（M-1: 统一 overlay 状态查询入口）
// ============================================================================

TEST_CASE("Terminal is_overlay_active defaults to false", "[terminal][overlay][init]") {
    // 未 initialize() 的 Terminal，overlay 状态应为 false
    REQUIRE_FALSE(null_terminal.is_overlay_active());
}

TEST_CASE("Terminal begin_overlay is safe without display_buffer", "[terminal][overlay][safety]") {
    // 未 initialize() 时 m_display_buffer 为 nullptr，begin_overlay 应安全跳过
    // 不设置 overlay_active，不崩溃
    null_terminal.begin_overlay(1, 10);
    REQUIRE_FALSE(null_terminal.is_overlay_active());

    // end_overlay 在未激活时也应安全
    null_terminal.end_overlay();
    REQUIRE_FALSE(null_terminal.is_overlay_active());
}

// ============================================================================
// DisplayBuffer 快照机制（overlay begin/end 的基础组件）
// ============================================================================

TEST_CASE("DisplayBuffer snapshot for overlay restore", "[display_buffer][overlay]") {
    // 模拟 overlay 机制：feed 内容 → snapshot → 清屏 → 从 snapshot 恢复
    DisplayBuffer db(100);
    db.set_width(80);
    db.set_height(6);

    // 写入对话内容
    db.feed("Line 1\nLine 2\nLine 3\n");
    REQUIRE(db.row_count() == 3);

    SECTION("snapshot captures full range") {
        // begin_overlay 时快照指定行范围
        auto snap = db.snapshot(1, 3);
        REQUIRE(snap.size() == 3);
        REQUIRE(snap[0] == "Line 1");
        REQUIRE(snap[1] == "Line 2");
        REQUIRE(snap[2] == "Line 3");
    }

    SECTION("snapshot of partial range") {
        auto snap = db.snapshot(2, 3);
        REQUIRE(snap.size() == 2);
        REQUIRE(snap[0] == "Line 2");
        REQUIRE(snap[1] == "Line 3");
    }

    SECTION("snapshot of empty rows") {
        // 超出已填充行数的部分返回空字符串
        auto snap = db.snapshot(1, 5);
        REQUIRE(snap.size() == 5);
        REQUIRE(snap[3].empty());
        REQUIRE(snap[4].empty());
    }
}

TEST_CASE("DisplayBuffer snapshot preserves SGR for overlay restore", "[display_buffer][overlay][sgr]") {
    // overlay 恢复时需要自包含的 SGR（leading SGR + 文本）
    DisplayBuffer db(100);
    db.set_width(80);
    db.set_height(4);

    // 模拟 set_color + write + reset_color 的字节流
    db.feed("\x1b[32mGreen text\x1b[0m\n");
    REQUIRE(db.row_count() == 1);

    auto snap = db.snapshot(1, 1);
    REQUIRE(snap.size() == 1);
    // 快照行应自包含 SGR，可直接重发恢复颜色
    REQUIRE_THAT(snap[0], Catch::Matchers::ContainsSubstring("Green text"));
    REQUIRE_THAT(snap[0], Catch::Matchers::ContainsSubstring("\x1b[32m"));
}

TEST_CASE("DisplayBuffer snapshot after clear_all for overlay re-render", "[display_buffer][overlay][clear]") {
    // 模拟展开思考视图时的清屏场景
    DisplayBuffer db(100);
    db.set_width(80);
    db.set_height(4);

    db.feed("Old content\n");
    REQUIRE(db.row_count() == 1);

    // 清屏（模拟 \x1b[2J）
    db.clear_all();
    REQUIRE(db.row_count() == 0);

    // 清屏后快照应为空
    auto snap = db.snapshot(1, 4);
    for (const auto& row : snap) {
        REQUIRE(row.empty());
    }
}
