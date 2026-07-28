/**
 * @file test_tool_kind.cpp
 * @brief ToolType 枚举与 infer_tool_type 纯函数测试
 * @details L-1：infer_tool_type 原位于 chat_session.cpp 匿名命名空间，
 *          无法被其他模块或测试复用。现提升为 core/tool_kind.h/.cpp 的公共纯函数，
 *          本文件验证其行为契约。
 */

#include <catch2/catch_test_macros.hpp>
#include <string_view>

#include "core/tool_kind.h"

using namespace agent::tool;

// ============================================================================
// L-1：infer_tool_type 公共纯函数测试
// ============================================================================

TEST_CASE("infer_tool_type returns ReadFile for Read", "[tool_kind][l1]") {
    REQUIRE(infer_tool_type("Read") == ToolType::ReadFile);
}

TEST_CASE("infer_tool_type returns WriteFile for Write", "[tool_kind][l1]") {
    REQUIRE(infer_tool_type("Write") == ToolType::WriteFile);
}

TEST_CASE("infer_tool_type returns EditFile for Edit", "[tool_kind][l1]") {
    REQUIRE(infer_tool_type("Edit") == ToolType::EditFile);
}

TEST_CASE("infer_tool_type returns Execute for Bash", "[tool_kind][l1]") {
    REQUIRE(infer_tool_type("Bash") == ToolType::Execute);
}

TEST_CASE("infer_tool_type returns Search for Grep and Glob", "[tool_kind][l1]") {
    REQUIRE(infer_tool_type("Grep") == ToolType::Search);
    REQUIRE(infer_tool_type("Glob") == ToolType::Search);
}

TEST_CASE("infer_tool_type returns Agent for Agent", "[tool_kind][l1]") {
    REQUIRE(infer_tool_type("Agent") == ToolType::Agent);
}

TEST_CASE("infer_tool_type returns Other for unknown tool names", "[tool_kind][l1]") {
    REQUIRE(infer_tool_type("UnknownTool") == ToolType::Other);
    REQUIRE(infer_tool_type("") == ToolType::Other);
    REQUIRE(infer_tool_type("read") == ToolType::Other);  // 大小写敏感
}

TEST_CASE("infer_tool_type accepts string_view", "[tool_kind][l1]") {
    // L-1：签名改为 std::string_view，验证从 const char* / std::string / std::string_view 均可调用
    std::string name_str = "Read";
    std::string_view name_sv = "Read";
    REQUIRE(infer_tool_type("Read") == ToolType::ReadFile);
    REQUIRE(infer_tool_type(name_str) == ToolType::ReadFile);
    REQUIRE(infer_tool_type(name_sv) == ToolType::ReadFile);
}

TEST_CASE("infer_tool_type is pure (no side effects)", "[tool_kind][l1]") {
    // L-1：纯函数契约 — 多次调用相同输入应返回相同结果，且不修改输入
    std::string name = "Bash";
    auto r1 = infer_tool_type(name);
    auto r2 = infer_tool_type(name);
    REQUIRE(r1 == r2);
    REQUIRE(r1 == ToolType::Execute);
    REQUIRE(name == "Bash");  // 入参未被修改
}
