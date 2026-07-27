/**
 * @file test_tool_context.cpp
 * @brief ToolContext 单元测试
 * @details H-5：验证 config_manager() 不再回退单例，nullptr 时抛 std::logic_error。
 *          覆盖 cancel_flag/cancelled_ 等其他成员的边界场景。
 */

#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <stdexcept>

#include "agent/tool/context.h"
#include "core/config/i_config_manager.h"
#include "core/config/config_manager.h"

using namespace agent::tool;
using namespace agent;

// ============================================================
// H-5: config_manager() 强制 DI 注入
// ============================================================

TEST_CASE("ToolContext config_manager throws on null (H-5)", "[tool][context][h5]") {
    ToolContext ctx;
    ctx.config_manager_ptr = nullptr;

    REQUIRE_THROWS_AS(ctx.config_manager(), std::logic_error);
}

TEST_CASE("ToolContext config_manager returns injected instance (H-5)", "[tool][context][h5]") {
    ToolContext ctx;
    auto& cfg = ConfigManager::instance();
    ctx.config_manager_ptr = &cfg;

    REQUIRE(&ctx.config_manager() == &cfg);
}

TEST_CASE("ToolContext config_manager default ptr is null", "[tool][context][h5]") {
    ToolContext ctx;
    // 默认值应为 nullptr（H-5：调用方必须显式注入）
    REQUIRE(ctx.config_manager_ptr == nullptr);
}

// ============================================================
// 取消信号（非 H-5 范畴，但顺便覆盖）
// ============================================================

TEST_CASE("ToolContext is_cancelled false by default", "[tool][context]") {
    ToolContext ctx;
    REQUIRE_FALSE(ctx.is_cancelled());
}

TEST_CASE("ToolContext is_cancelled true after cancel without flag", "[tool][context]") {
    ToolContext ctx;
    ctx.cancel_flag = nullptr;
    ctx.cancel();
    REQUIRE(ctx.is_cancelled());
}

TEST_CASE("ToolContext external cancel_flag respected", "[tool][context]") {
    ToolContext ctx;
    std::atomic<bool> ext_flag{false};
    ctx.cancel_flag = &ext_flag;

    // 外部置位前：未取消
    REQUIRE_FALSE(ctx.is_cancelled());

    // 外部置位
    ext_flag.store(true);
    REQUIRE(ctx.is_cancelled());

    // cancel() 在绑定外部 flag 时无操作（由外部负责置位）
    ctx.cancel();
    REQUIRE(ctx.is_cancelled());  // 仍为 true（外部 flag 状态）
}
