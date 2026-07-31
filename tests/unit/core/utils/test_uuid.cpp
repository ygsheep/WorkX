/**
 * @file test_uuid.cpp
 * @brief UUIDv4 生成工具单元测试
 */

#include <catch2/catch_test_macros.hpp>
#include <regex>
#include <set>

#include "core/utils/uuid.h"

TEST_CASE("uuid: generates valid UUIDv4 format", "[core][utils][uuid]") {
    std::string id = core::util::generate_uuid();
    // UUIDv4 格式：xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx（y 为 8/9/a/b）
    std::regex re("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$");
    REQUIRE(std::regex_match(id, re));
}

TEST_CASE("uuid: generates unique values", "[core][utils][uuid]") {
    std::string a = core::util::generate_uuid();
    std::string b = core::util::generate_uuid();
    REQUIRE(a != b);
}

TEST_CASE("uuid: empty string never returned", "[core][utils][uuid]") {
    for (int i = 0; i < 100; ++i) {
        REQUIRE_FALSE(core::util::generate_uuid().empty());
    }
}

TEST_CASE("uuid: 1000 unique checks (collision sanity)", "[core][utils][uuid]") {
    std::set<std::string> seen;
    for (int i = 0; i < 1000; ++i) {
        seen.insert(core::util::generate_uuid());
    }
    REQUIRE(seen.size() == 1000);
}
