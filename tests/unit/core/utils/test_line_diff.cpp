/**
 * @file test_line_diff.cpp
 * @brief line_diff 行级 LCS diff 单元测试
 */

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "core/utils/line_diff.h"

using namespace agent;

namespace {

std::vector<std::string> L(std::initializer_list<const char*> lines) {
    std::vector<std::string> out;
    out.reserve(lines.size());
    for (const char* s : lines) out.emplace_back(s);
    return out;
}

}  // namespace

TEST_CASE("line_diff empty inputs produce empty result", "[core][line_diff]") {
    REQUIRE(line_diff({}, {}).empty());
}

TEST_CASE("line_diff all-new content is all Insert", "[core][line_diff]") {
    auto d = line_diff({}, L({"a", "b", "c"}));
    REQUIRE(d.size() == 3);
    REQUIRE(d[0].kind == DiffKind::Insert);
    REQUIRE(d[0].text == "a");
    REQUIRE(d[0].line_no == 1);
    REQUIRE(d[1].kind == DiffKind::Insert);
    REQUIRE(d[1].line_no == 2);
    REQUIRE(d[2].kind == DiffKind::Insert);
    REQUIRE(d[2].line_no == 3);
}

TEST_CASE("line_diff all-deleted content produces empty result", "[core][line_diff]") {
    // 仅删除：不渲染删除行，输出为空
    REQUIRE(line_diff(L({"a", "b"}), {}).empty());
}

TEST_CASE("line_diff identical content is all Equal", "[core][line_diff]") {
    auto d = line_diff(L({"a", "b"}), L({"a", "b"}));
    REQUIRE(d.size() == 2);
    REQUIRE(d[0].kind == DiffKind::Equal);
    REQUIRE(d[0].line_no == 1);
    REQUIRE(d[1].kind == DiffKind::Equal);
    REQUIRE(d[1].line_no == 2);
}

TEST_CASE("line_diff interleaved modify keeps Equal context", "[core][line_diff]") {
    auto d = line_diff(L({"a", "b", "c"}), L({"a", "x", "c"}));
    REQUIRE(d.size() == 3);
    REQUIRE(d[0].kind == DiffKind::Equal);
    REQUIRE(d[0].text == "a");
    REQUIRE(d[1].kind == DiffKind::Modify);
    REQUIRE(d[1].text == "x");
    REQUIRE(d[1].line_no == 2);
    REQUIRE(d[2].kind == DiffKind::Equal);
    REQUIRE(d[2].text == "c");
}

TEST_CASE("line_diff replace pairs removes with adds as Modify", "[core][line_diff]") {
    // 2 行替换 2 行：全部 Modify，无 Insert
    auto d = line_diff(L({"a", "b"}), L({"x", "y"}));
    REQUIRE(d.size() == 2);
    REQUIRE(d[0].kind == DiffKind::Modify);
    REQUIRE(d[0].text == "x");
    REQUIRE(d[1].kind == DiffKind::Modify);
    REQUIRE(d[1].text == "y");
}

TEST_CASE("line_diff partial replace yields Modify + Insert", "[core][line_diff]") {
    // 1 行替换 2 行：Modify + Insert
    auto d = line_diff(L({"a"}), L({"x", "y"}));
    REQUIRE(d.size() == 2);
    REQUIRE(d[0].kind == DiffKind::Modify);
    REQUIRE(d[0].text == "x");
    REQUIRE(d[1].kind == DiffKind::Insert);
    REQUIRE(d[1].text == "y");
}

TEST_CASE("line_diff new_start offsets line numbers", "[core][line_diff]") {
    auto d = line_diff(L({"a", "b"}), L({"a", "x"}), /*new_start=*/10);
    REQUIRE(d.size() == 2);
    REQUIRE(d[0].kind == DiffKind::Equal);
    REQUIRE(d[0].line_no == 10);
    REQUIRE(d[1].kind == DiffKind::Modify);
    REQUIRE(d[1].line_no == 11);
}

TEST_CASE("line_diff large input degrades to all Insert", "[core][line_diff]") {
    // 超过 LCS 阈值（5000 行）：降级为全 Insert，不崩溃、行号连续
    std::vector<std::string> old_lines;
    std::vector<std::string> new_lines;
    for (int i = 0; i < 6000; ++i) {
        old_lines.push_back("old" + std::to_string(i));
        new_lines.push_back("new" + std::to_string(i));
    }
    auto d = line_diff(old_lines, new_lines);
    REQUIRE(d.size() == new_lines.size());
    REQUIRE(d.front().kind == DiffKind::Insert);
    REQUIRE(d.front().line_no == 1);
    REQUIRE(d.back().kind == DiffKind::Insert);
    REQUIRE(d.back().line_no == 6000);
}
