/**
 * @file test_tool_registry.cpp
 * @brief ToolRegistry 单元测试
 * @details 覆盖 register_tool/find_by_name/get_all_tools/get_all_schemas/exists/size
 */

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>

#include "agent/tool/registry.h"
#include "agent/tool/itool.h"
#include "agent/tool/result.h"

using namespace agent;
using namespace agent::tool;

namespace {

class StubTool : public ITool {
public:
    StubTool(std::string n, std::string d = "", nlohmann::json schema = {{"type", "object"}})
        : name_(std::move(n)), desc_(std::move(d)), schema_(std::move(schema)) {}

    const std::string& name() const override { return name_; }
    const std::string& description() const override { return desc_; }
    const std::string& prompt() const override { static const std::string p; return p; }
    nlohmann::json input_schema() const override { return schema_; }
    ResultV2<ToolResult> call(const nlohmann::json&, const ToolContext&) const override {
        return ResultV2<ToolResult>::ok(ToolResult::ok(std::string("ok")));
    }

private:
    std::string name_;
    std::string desc_;
    nlohmann::json schema_;
};

} // namespace

// ============================================================================
// register_tool
// ============================================================================

TEST_CASE("ToolRegistry register_tool adds tool", "[tool_registry][register]") {
    ToolRegistry registry;
    REQUIRE(registry.size() == 0);

    registry.register_tool(std::make_shared<StubTool>("ToolA"));
    REQUIRE(registry.size() == 1);

    registry.register_tool(std::make_shared<StubTool>("ToolB"));
    REQUIRE(registry.size() == 2);
}

TEST_CASE("ToolRegistry register_tool ignores null", "[tool_registry][register]") {
    ToolRegistry registry;
    registry.register_tool(nullptr);
    REQUIRE(registry.size() == 0);
}

TEST_CASE("ToolRegistry register_tool allows duplicate name (last wins)", "[tool_registry][register]") {
    ToolRegistry registry;
    registry.register_tool(std::make_shared<StubTool>("Dup"));
    registry.register_tool(std::make_shared<StubTool>("Dup"));

    // 当前实现允许同名注册，name_index_ 后者覆盖前者
    REQUIRE(registry.size() == 2);  // tools_ 列表有两个
    REQUIRE(registry.exists("Dup"));
    auto found = registry.find_by_name("Dup");
    REQUIRE(found != nullptr);
}

// ============================================================================
// find_by_name
// ============================================================================

TEST_CASE("ToolRegistry find_by_name returns registered tool", "[tool_registry][find]") {
    ToolRegistry registry;
    registry.register_tool(std::make_shared<StubTool>("Findable"));

    auto tool = registry.find_by_name("Findable");
    REQUIRE(tool != nullptr);
    REQUIRE(tool->name() == "Findable");
}

TEST_CASE("ToolRegistry find_by_name returns nullptr for unknown", "[tool_registry][find]") {
    ToolRegistry registry;
    registry.register_tool(std::make_shared<StubTool>("A"));

    auto tool = registry.find_by_name("NonExistent");
    REQUIRE(tool == nullptr);
}

TEST_CASE("ToolRegistry find_by_name on empty registry returns nullptr", "[tool_registry][find]") {
    ToolRegistry registry;
    REQUIRE(registry.find_by_name("Anything") == nullptr);
}

// ============================================================================
// get_all_tools
// ============================================================================

TEST_CASE("ToolRegistry get_all_tools returns all registered", "[tool_registry][all]") {
    ToolRegistry registry;
    registry.register_tool(std::make_shared<StubTool>("A"));
    registry.register_tool(std::make_shared<StubTool>("B"));
    registry.register_tool(std::make_shared<StubTool>("C"));

    auto tools = registry.get_all_tools();
    REQUIRE(tools.size() == 3);
    REQUIRE(tools[0]->name() == "A");
    REQUIRE(tools[1]->name() == "B");
    REQUIRE(tools[2]->name() == "C");
}

TEST_CASE("ToolRegistry get_all_tools on empty returns empty vector", "[tool_registry][all]") {
    ToolRegistry registry;
    auto tools = registry.get_all_tools();
    REQUIRE(tools.empty());
}

// ============================================================================
// get_all_schemas
// ============================================================================

TEST_CASE("ToolRegistry get_all_schemas returns JSON array", "[tool_registry][schema]") {
    ToolRegistry registry;
    registry.register_tool(std::make_shared<StubTool>(
        "ToolA", "Description A", R"({"type":"object","properties":{"x":{"type":"string"}}})"_json));

    auto schemas = registry.get_all_schemas();

    REQUIRE(schemas.is_array());
    REQUIRE(schemas.size() == 1);
    REQUIRE(schemas[0]["name"] == "ToolA");
    REQUIRE(schemas[0]["description"] == "Description A");
    REQUIRE(schemas[0]["input_schema"]["type"] == "object");
}

TEST_CASE("ToolRegistry get_all_schemas for multiple tools", "[tool_registry][schema]") {
    ToolRegistry registry;
    registry.register_tool(std::make_shared<StubTool>("A", "desc A"));
    registry.register_tool(std::make_shared<StubTool>("B", "desc B"));

    auto schemas = registry.get_all_schemas();

    REQUIRE(schemas.size() == 2);
    REQUIRE(schemas[0]["name"] == "A");
    REQUIRE(schemas[1]["name"] == "B");
}

TEST_CASE("ToolRegistry get_all_schemas on empty returns empty array", "[tool_registry][schema]") {
    ToolRegistry registry;
    auto schemas = registry.get_all_schemas();
    REQUIRE(schemas.is_array());
    REQUIRE(schemas.empty());
}

// ============================================================================
// exists & size
// ============================================================================

TEST_CASE("ToolRegistry exists returns true for registered", "[tool_registry][exists]") {
    ToolRegistry registry;
    registry.register_tool(std::make_shared<StubTool>("Exists"));

    REQUIRE(registry.exists("Exists"));
    REQUIRE_FALSE(registry.exists("DoesNotExist"));
}

TEST_CASE("ToolRegistry size returns correct count", "[tool_registry][size]") {
    ToolRegistry registry;
    REQUIRE(registry.size() == 0);

    registry.register_tool(std::make_shared<StubTool>("A"));
    REQUIRE(registry.size() == 1);

    registry.register_tool(std::make_shared<StubTool>("B"));
    registry.register_tool(std::make_shared<StubTool>("C"));
    REQUIRE(registry.size() == 3);
}
