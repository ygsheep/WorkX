/**
 * @file test_suggest_logic.cpp
 * @brief 输入栏提示面板 / 聚合搜索面板无头逻辑单元测试
 * @details 覆盖："/" 与 "@" 前缀模式推导（对齐 bottom_bar 语义）、
 *          命令过滤（大小写不敏感 + 多字段匹配 + 保持顺序）、
 *          聚合搜索过滤（空查询全量、标题前缀命中优先、类别标签齐全）。
 * @note 测试名用英文：Windows 上 CMake catch_discover_tests 对 GBK 管道
 *       捕获 UTF-8 中文名会损坏 JSON（既有环境行为）。
 */

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "widgets/search_palette.h"
#include "widgets/suggest_panel.h"

using namespace ftxtui;

// ============================================================================
// parse_suggest_query："/" 命令模式
// ============================================================================

TEST_CASE("parse_suggest_query leading slash enters command mode", "[suggest][parse]") {
    std::string q;
    REQUIRE(parse_suggest_query("/mod", q) == SuggestMode::Command);
    REQUIRE(q == "mod");
}

TEST_CASE("parse_suggest_query bare slash gives empty query", "[suggest][parse]") {
    std::string q;
    REQUIRE(parse_suggest_query("/", q) == SuggestMode::Command);
    REQUIRE(q.empty());
}

TEST_CASE("parse_suggest_query no mode without leading slash", "[suggest][parse]") {
    std::string q;
    REQUIRE(parse_suggest_query("hello", q) == SuggestMode::None);
    REQUIRE(parse_suggest_query("a/b", q) == SuggestMode::None);  // 仅行首生效
}

// ============================================================================
// parse_suggest_query："@" 文件模式（行内最后一个 @ 且其后无空格）
// ============================================================================

TEST_CASE("parse_suggest_query inline at enters file mode", "[suggest][parse]") {
    std::string q;
    REQUIRE(parse_suggest_query("看看 @src", q) == SuggestMode::File);
    REQUIRE(q == "src");
}

TEST_CASE("parse_suggest_query leading at is file mode too", "[suggest][parse]") {
    std::string q;
    REQUIRE(parse_suggest_query("@src/core", q) == SuggestMode::File);
    REQUIRE(q == "src/core");
}

TEST_CASE("parse_suggest_query at followed by space exits file mode", "[suggest][parse]") {
    std::string q;
    REQUIRE(parse_suggest_query("聊聊 @ src", q) == SuggestMode::None);
}

TEST_CASE("parse_suggest_query last at wins as query start", "[suggest][parse]") {
    std::string q;
    REQUIRE(parse_suggest_query("a @b 看 @c", q) == SuggestMode::File);
    REQUIRE(q == "c");
}

TEST_CASE("parse_suggest_query leading slash beats inline at", "[suggest][parse]") {
    std::string q;
    REQUIRE(parse_suggest_query("/mod @x", q) == SuggestMode::Command);
    REQUIRE(q == "mod @x");
}

// ============================================================================
// filter_commands：子串过滤
// ============================================================================

TEST_CASE("filter_commands empty query returns all", "[suggest][filter]") {
    const std::vector<std::string> cmds = {"/clear", "/model", "/help"};
    REQUIRE(filter_commands(cmds, "").size() == 3);
}

TEST_CASE("filter_commands is case insensitive", "[suggest][filter]") {
    const std::vector<std::string> cmds = {"/clear", "/model"};
    auto hits = filter_commands(cmds, "CLEAR");
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0] == 0);
}

TEST_CASE("filter_commands matches substring not prefix", "[suggest][filter]") {
    const std::vector<std::string> cmds = {"/clear", "/model"};
    auto hits = filter_commands(cmds, "le");
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0] == 0);  // "/clear" 中间子串 "le"，非前缀
}

TEST_CASE("filter_commands no match returns empty", "[suggest][filter]") {
    const std::vector<std::string> cmds = {"/clear", "/model"};
    REQUIRE(filter_commands(cmds, "zzz").empty());
}

TEST_CASE("filter_commands keeps source order", "[suggest][filter]") {
    const std::vector<std::string> cmds = {"/clear", "/model"};
    auto hits = filter_commands(cmds, "e");
    REQUIRE(hits.size() == 2);
    REQUIRE(hits[0] == 0);
    REQUIRE(hits[1] == 1);
}

// ============================================================================
// filter_search_entries：聚合搜索过滤
// ============================================================================

static SearchEntry make_entry(SearchCategory c, std::string title) {
    SearchEntry e;
    e.category = c;
    e.title = std::move(title);
    return e;
}

TEST_CASE("filter_search_entries empty query returns all", "[palette][filter]") {
    std::vector<SearchEntry> entries = {
        make_entry(SearchCategory::Feature, "切换模型"),
        make_entry(SearchCategory::File, "app.cpp"),
        make_entry(SearchCategory::Session, "昨天的工作"),
        make_entry(SearchCategory::Setting, "自动滚动"),
    };
    REQUIRE(filter_search_entries(entries, "").size() == 4);
}

TEST_CASE("filter_search_entries title prefix hits rank first", "[palette][filter]") {
    std::vector<SearchEntry> entries = {
        make_entry(SearchCategory::Feature, "新会话"),
        make_entry(SearchCategory::Setting, "会话列表上限"),
    };
    // "会话" 命中第二条的标题前缀，应排到第一条前
    auto hits = filter_search_entries(entries, "会话");
    REQUIRE(hits.size() == 2);
    REQUIRE(hits[0] == 1);
    REQUIRE(hits[1] == 0);
}

TEST_CASE("filter_search_entries keywords field participates", "[palette][filter]") {
    std::vector<SearchEntry> entries = {
        make_entry(SearchCategory::Feature, "清空对话"),
    };
    entries[0].keywords = "clear";
    auto hits = filter_search_entries(entries, "clear");
    REQUIRE(hits.size() == 1);
    REQUIRE(hits[0] == 0);
}

TEST_CASE("filter_search_entries is case insensitive", "[palette][filter]") {
    std::vector<SearchEntry> entries = {
        make_entry(SearchCategory::File, "src/core/utils.cpp"),
    };
    REQUIRE(filter_search_entries(entries, "CORE").size() == 1);
    REQUIRE(filter_search_entries(entries, "UTILS").size() == 1);
}

TEST_CASE("filter_search_entries no match returns empty", "[palette][filter]") {
    std::vector<SearchEntry> entries = {
        make_entry(SearchCategory::Feature, "切换模型"),
    };
    REQUIRE(filter_search_entries(entries, "不存在的内容").empty());
}

TEST_CASE("filter_search_entries keeps source order for substring hits", "[palette][filter]") {
    std::vector<SearchEntry> entries = {
        make_entry(SearchCategory::Session, "第一场"),
        make_entry(SearchCategory::Feature, "第二个"),
        make_entry(SearchCategory::File, "第三场"),
    };
    auto hits = filter_search_entries(entries, "场");
    REQUIRE(hits.size() == 2);
    REQUIRE(hits[0] == 0);
    REQUIRE(hits[1] == 2);
}