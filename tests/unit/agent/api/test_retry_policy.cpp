/**
 * @file test_retry_policy.cpp
 * @brief HttpRetryPolicy 单元测试（H-3）
 * @details 验证重试策略的核心行为：
 *          - is_retryable: HTTP 状态码 + 错误消息判断
 *          - delay: 指数退避 + 上限保护
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include "agent/api/retry.h"

#include <chrono>

using namespace agent;

// ============================================================
// is_retryable
// ============================================================

TEST_CASE("HttpRetryPolicy is_retryable HTTP 429", "[retry][is_retryable]") {
    REQUIRE(HttpRetryPolicy::is_retryable(429, "") == true);
}

TEST_CASE("HttpRetryPolicy is_retryable HTTP 5xx", "[retry][is_retryable]") {
    REQUIRE(HttpRetryPolicy::is_retryable(500, "") == true);
    REQUIRE(HttpRetryPolicy::is_retryable(502, "") == true);
    REQUIRE(HttpRetryPolicy::is_retryable(503, "") == true);
    REQUIRE(HttpRetryPolicy::is_retryable(504, "") == true);
    REQUIRE(HttpRetryPolicy::is_retryable(599, "") == true);
}

TEST_CASE("HttpRetryPolicy is_retryable HTTP 4xx (except 429)", "[retry][is_retryable]") {
    REQUIRE(HttpRetryPolicy::is_retryable(400, "") == false);
    REQUIRE(HttpRetryPolicy::is_retryable(401, "") == false);
    REQUIRE(HttpRetryPolicy::is_retryable(403, "") == false);
    REQUIRE(HttpRetryPolicy::is_retryable(404, "") == false);
}

TEST_CASE("HttpRetryPolicy is_retryable HTTP 2xx", "[retry][is_retryable]") {
    REQUIRE(HttpRetryPolicy::is_retryable(200, "") == false);
    REQUIRE(HttpRetryPolicy::is_retryable(201, "") == false);
    REQUIRE(HttpRetryPolicy::is_retryable(204, "") == false);
}

TEST_CASE("HttpRetryPolicy is_retryable network error (status=0)", "[retry][is_retryable]") {
    // status=0 + 非空 error_msg 表示网络错误（curl 错误），可重试
    REQUIRE(HttpRetryPolicy::is_retryable(0, "Connection timed out") == true);
    REQUIRE(HttpRetryPolicy::is_retryable(0, "DNS resolution failed") == true);
    // status=0 + 空 error_msg 不可重试（无法判断错误类型）
    REQUIRE(HttpRetryPolicy::is_retryable(0, "") == false);
}

TEST_CASE("HttpRetryPolicy is_retryable max iterations", "[retry][is_retryable]") {
    // 业务逻辑错误：max iterations 不可重试
    REQUIRE(HttpRetryPolicy::is_retryable(0, "max iterations exceeded") == false);
    REQUIRE(HttpRetryPolicy::is_retryable(500, "max iterations reached") == false);
}

// ============================================================
// delay (指数退避)
// ============================================================

TEST_CASE("HttpRetryPolicy delay exponential backoff", "[retry][delay]") {
    HttpRetryPolicy policy{.max_retries = 5, .base_delay_ms = 1000, .max_delay_ms = 60000};

    // attempt 0: 1000 * 2^0 = 1000
    REQUIRE(policy.delay_ms(0) == 1000);
    // attempt 1: 1000 * 2^1 = 2000
    REQUIRE(policy.delay_ms(1) == 2000);
    // attempt 2: 1000 * 2^2 = 4000
    REQUIRE(policy.delay_ms(2) == 4000);
    // attempt 3: 1000 * 2^3 = 8000
    REQUIRE(policy.delay_ms(3) == 8000);
    // attempt 4: 1000 * 2^4 = 16000
    REQUIRE(policy.delay_ms(4) == 16000);
}

TEST_CASE("HttpRetryPolicy delay max cap", "[retry][delay]") {
    HttpRetryPolicy policy{.max_retries = 10, .base_delay_ms = 1000, .max_delay_ms = 60000};

    // attempt 6: 1000 * 2^6 = 64000 → 截断为 60000
    REQUIRE(policy.delay_ms(6) == 60000);
    // attempt 10: 远超上限，仍为 60000
    REQUIRE(policy.delay_ms(10) == 60000);
}

TEST_CASE("HttpRetryPolicy delay returns chrono::milliseconds", "[retry][delay]") {
    HttpRetryPolicy policy{.base_delay_ms = 500};

    auto d = policy.delay(2);
    REQUIRE(d == std::chrono::milliseconds(2000));
}

TEST_CASE("HttpRetryPolicy delay int overflow safety", "[retry][delay]") {
    // H-3 重点：用 64 位计算避免 1<<attempt 在 attempt=30+ 时 int 溢出（UB）
    HttpRetryPolicy policy{.base_delay_ms = 1000, .max_delay_ms = 60000};

    // attempt=30 时 1<<30 = 1073741824，乘以 1000 远超 int 范围
    // 但用 int64_t 计算后会被 max_delay_ms 截断为 60000
    REQUIRE(policy.delay_ms(30) == 60000);
    REQUIRE(policy.delay_ms(40) == 60000);
}

// ============================================================
// 默认值
// ============================================================

TEST_CASE("HttpRetryPolicy default values", "[retry][defaults]") {
    HttpRetryPolicy policy{};

    REQUIRE(policy.max_retries == 3);
    REQUIRE(policy.base_delay_ms == 1000);
    REQUIRE(policy.max_delay_ms == 60000);
}
