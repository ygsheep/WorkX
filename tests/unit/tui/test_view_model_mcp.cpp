/**
 * @file test_view_model_mcp.cpp
 * @brief #27 M4：ActionMcpStatus 更新 SidebarModel.mcp_servers（触发重绘）
 * @date 2026-08
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <vector>

#include "bridge/action.h"
#include "vm/view_model.h"

using namespace ftxtui;
using namespace Catch::Matchers;

TEST_CASE("ViewModel ActionMcpStatus stores formatted server lines", "[view_model][mcp]") {
    ViewModel vm;
    REQUIRE(vm.sidebar.mcp_servers.empty());

    std::vector<McpServerLite> servers = {
        {"github", "2026-07-28", 3},
        {"notion", "2025-11-25", 0},
    };
    REQUIRE(vm.apply(ActionMcpStatus{.servers = servers}));
    REQUIRE(vm.sidebar.mcp_servers.size() == 2);
    REQUIRE_THAT(vm.sidebar.mcp_servers[0], ContainsSubstring("github"));
    REQUIRE_THAT(vm.sidebar.mcp_servers[0], ContainsSubstring("2026-07-28"));
    REQUIRE_THAT(vm.sidebar.mcp_servers[0], ContainsSubstring("3 工具"));
    // 无工具时不显示工具数
    REQUIRE_THAT(vm.sidebar.mcp_servers[1], ContainsSubstring("notion"));
    REQUIRE_THAT(vm.sidebar.mcp_servers[1], ContainsSubstring("2025-11-25"));
    REQUIRE_THAT(vm.sidebar.mcp_servers[1], !ContainsSubstring("工具"));
}

TEST_CASE("ViewModel ActionMcpStatus identical snapshot returns false", "[view_model][mcp]") {
    ViewModel vm;
    std::vector<McpServerLite> servers = {{"github", "2026-07-28", 2}};
    REQUIRE(vm.apply(ActionMcpStatus{.servers = servers}));
    // 相同快照 → 无变化，不触发重绘
    REQUIRE_FALSE(vm.apply(ActionMcpStatus{.servers = servers}));
}

TEST_CASE("ViewModel ActionMcpStatus empty snapshot clears servers", "[view_model][mcp]") {
    ViewModel vm;
    std::vector<McpServerLite> servers = {{"github", "2026-07-28", 2}};
    vm.apply(ActionMcpStatus{.servers = servers});
    REQUIRE(vm.apply(ActionMcpStatus{.servers = {}}));
    REQUIRE(vm.sidebar.mcp_servers.empty());
}
