/**
 * @file test_token_stats_model.cpp
 * @brief TokenStatsModel 单元测试
 * @details 覆盖用户输入估算、StreamDone usage 更新、响应内容估算、
 *          cache 命中统计、消息计数、reset 等场景
 */

#include <catch2/catch_test_macros.hpp>

#include "tui/model/token_stats_model.h"

using namespace tui;

// ============================================================================
// 初始状态
// ============================================================================

TEST_CASE("TokenStatsModel starts at zero", "[token_stats][init]") {
    TokenStatsModel m;
    REQUIRE(m.message_count() == 0);
    REQUIRE(m.total_tokens() == 0);
    REQUIRE(m.cache_read_tokens() == 0);
}

// ============================================================================
// add_user_input
// ============================================================================

TEST_CASE("TokenStatsModel add_user_input empty text no-op", "[token_stats][user_input]") {
    TokenStatsModel m;
    m.add_user_input("");
    REQUIRE(m.total_tokens() == 0);
}

TEST_CASE("TokenStatsModel add_user_input accumulates tokens", "[token_stats][user_input]") {
    TokenStatsModel m;
    m.add_user_input("hello world");
    REQUIRE(m.total_tokens() > 0);
    int32_t first = m.total_tokens();
    m.add_user_input("another message");
    REQUIRE(m.total_tokens() > first);
}

// ============================================================================
// update_from_usage
// ============================================================================

TEST_CASE("TokenStatsModel update_from_usage overwrites total", "[token_stats][usage]") {
    TokenStatsModel m;
    m.add_user_input("preset content");
    REQUIRE(m.total_tokens() > 0);

    // provider 返回 usage 应覆盖（而非累加）之前估算
    m.update_from_usage(100, 50, 20, 30);
    REQUIRE(m.total_tokens() == 200);  // 100 + 20 + 30 + 50
    REQUIRE(m.cache_read_tokens() == 30);
}

TEST_CASE("TokenStatsModel update_from_usage zero cache", "[token_stats][usage]") {
    TokenStatsModel m;
    m.update_from_usage(100, 50, 0, 0);
    REQUIRE(m.total_tokens() == 150);
    REQUIRE(m.cache_read_tokens() == 0);
}

// ============================================================================
// add_response_estimate
// ============================================================================

TEST_CASE("TokenStatsModel add_response_estimate accumulates and clears cache", "[token_stats][estimate]") {
    TokenStatsModel m;
    // 先设置 cache 命中
    m.update_from_usage(100, 50, 0, 30);
    REQUIRE(m.cache_read_tokens() == 30);

    // provider 不返回 usage 时估算响应内容，应清除 cache 显示
    m.add_response_estimate("response content", "reasoning");
    REQUIRE(m.total_tokens() > 150);  // 累加估算
    REQUIRE(m.cache_read_tokens() == 0);
}

// ============================================================================
// message_count
// ============================================================================

TEST_CASE("TokenStatsModel increment_message_count", "[token_stats][count]") {
    TokenStatsModel m;
    REQUIRE(m.message_count() == 0);
    m.increment_message_count();
    m.increment_message_count();
    REQUIRE(m.message_count() == 2);
}

// ============================================================================
// restore_from_history
// ============================================================================

TEST_CASE("TokenStatsModel restore_from_history estimates history tokens", "[token_stats][restore]") {
    TokenStatsModel m;
    // 先污染统计（模拟 /resume 前已有其他会话的残留值）
    m.add_user_input("residual content from previous session");
    m.update_from_usage(9000, 2000, 0, 1000);
    m.increment_message_count();
    REQUIRE(m.total_tokens() > 0);

    std::vector<agent::ChatMessage> history;
    history.push_back(agent::ChatMessage::user("hello world"));
    history.push_back(agent::ChatMessage::assistant("hi there response"));
    history.push_back(agent::ChatMessage::user("follow up question"));

    m.restore_from_history(history);

    REQUIRE(m.total_tokens() == agent::compact::estimate_messages_tokens(history));
    REQUIRE(m.total_tokens() > 0);
    REQUIRE(m.message_count() == static_cast<int32_t>(history.size()));
    REQUIRE(m.cache_read_tokens() == 0);
}

TEST_CASE("TokenStatsModel restore_from_history empty history resets to zero", "[token_stats][restore]") {
    TokenStatsModel m;
    m.add_user_input("content");
    m.update_from_usage(100, 50, 0, 30);
    m.increment_message_count();

    m.restore_from_history({});

    REQUIRE(m.total_tokens() == 0);
    REQUIRE(m.message_count() == 0);
    REQUIRE(m.cache_read_tokens() == 0);
}

// ============================================================================
// reset
// ============================================================================

TEST_CASE("TokenStatsModel reset clears all", "[token_stats][reset]") {
    TokenStatsModel m;
    m.add_user_input("content");
    m.update_from_usage(100, 50, 0, 30);
    m.increment_message_count();
    REQUIRE(m.total_tokens() > 0);
    REQUIRE(m.message_count() > 0);
    REQUIRE(m.cache_read_tokens() > 0);

    m.reset();
    REQUIRE(m.message_count() == 0);
    REQUIRE(m.total_tokens() == 0);
    REQUIRE(m.cache_read_tokens() == 0);
}

// ============================================================================
// 综合场景：模拟 ChatRenderer 事件序列
// ============================================================================

TEST_CASE("TokenStatsModel simulates full session flow", "[token_stats][integration]") {
    TokenStatsModel m;

    // 1. 用户输入
    m.add_user_input("hello");
    int32_t after_input = m.total_tokens();
    REQUIRE(after_input > 0);

    // 2. provider 返回 usage（覆盖估算）
    // total = prompt(10) + cache_creation(0) + cache_read(3) + generated(5) = 18
    m.update_from_usage(10, 5, 0, 3);
    REQUIRE(m.total_tokens() == 18);
    REQUIRE(m.cache_read_tokens() == 3);
    m.increment_message_count();

    // 3. 用户再次输入
    m.add_user_input("world");
    REQUIRE(m.total_tokens() > 18);  // 累加

    // 4. provider 不返回 usage（估算响应）
    m.add_response_estimate("response", "reasoning");
    REQUIRE(m.cache_read_tokens() == 0);  // 清除 cache
    m.increment_message_count();

    REQUIRE(m.message_count() == 2);
}
