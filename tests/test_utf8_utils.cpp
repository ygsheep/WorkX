/**
 * @file test_utf8_utils.cpp
 * @brief UTF-8 显示宽度工具单元测试
 */

#include <catch2/catch_test_macros.hpp>
#include "tui/utils/utf8_utils.h"

using namespace workx;

TEST_CASE("display_width ASCII", "[utf8]") {
    REQUIRE(display_width("hello") == 5);
    REQUIRE(display_width("") == 0);
    REQUIRE(display_width("a") == 1);
    REQUIRE(display_width(" ") == 1);
    REQUIRE(display_width("\t") == 1);  // tab counts as 1 (control exception)
}

TEST_CASE("display_width CJK", "[utf8]") {
    REQUIRE(display_width("\xe4\xb8\xad\xe6\x96\x87") == 4);  // "中文" = 2+2
    REQUIRE(display_width("abc\xe4\xb8\xad\xe6\x96\x87") == 7);  // "abc中文" = 3+4
    REQUIRE(display_width("\xe4\xb8\xad") == 2);  // "中" = 2
}

TEST_CASE("display_width emoji", "[utf8]") {
    // 🎉 = U+1F389 = F0 9F 8E 89 (4 bytes, width 2)
    REQUIRE(display_width("\xf0\x9f\x8e\x89") == 2);
    // "a🎉b" = 1+2+1 = 4
    REQUIRE(display_width("a\xf0\x9f\x8e\x89" "b") == 4);
}

TEST_CASE("display_width control characters", "[utf8]") {
    // ESC is a control char (width 0), but [31m are printable ASCII (width 1 each)
    // display_width counts character widths, not ANSI escape semantics
    REQUIRE(display_width("\x1b[31m") == 4);  // ESC(0) + [(1) + 3(1) + 1(1) + m(1)
    REQUIRE(display_width("\x1b[0m") == 3);   // ESC(0) + [(1) + 0(1) + m(1)
    // ESC alone has width 0
    REQUIRE(display_width("\x1b") == 0);
    // Other control chars
    REQUIRE(display_width("\x01\x02") == 0);  // SOH, STX
}

TEST_CASE("display_width box drawing", "[utf8]") {
    // Box-drawing characters are width 1 (not CJK)
    // │ = E2 94 82
    REQUIRE(display_width("\xe2\x94\x82") == 1);
    // ─ = E2 94 80
    REQUIRE(display_width("\xe2\x94\x80") == 1);
    // ┌┐└┘ = 4 box chars
    REQUIRE(display_width("\xe2\x94\x8c\xe2\x94\x90\xe2\x94\x94\xe2\x94\x98") == 4);
}

TEST_CASE("display_width invalid/truncated UTF-8", "[utf8]") {
    // Lone continuation byte — should not crash, width 1
    REQUIRE(display_width("\x80") == 1);
    // Truncated 3-byte sequence (only 2 bytes) — graceful handling
    REQUIRE(display_width("\xe4\xb8") == 1);
}

TEST_CASE("truncate_to_width no truncation", "[utf8]") {
    REQUIRE(truncate_to_width("hello", 10) == "hello");
    REQUIRE(truncate_to_width("hello", 5) == "hello");
    REQUIRE(truncate_to_width("", 5) == "");
    // Exact fit
    REQUIRE(truncate_to_width("hi", 2) == "hi");
}

TEST_CASE("truncate_to_width ASCII", "[utf8]") {
    // "hello" truncated to 3 → "he…"
    REQUIRE(truncate_to_width("hello", 3) == "he\xe2\x80\xa6");
    // "hello" truncated to 1 → "…"
    REQUIRE(truncate_to_width("hello", 1) == "\xe2\x80\xa6");
}

TEST_CASE("truncate_to_width CJK", "[utf8]") {
    // "中文" = width 4, truncate to 3 → "中…" (2+1=3)
    REQUIRE(truncate_to_width("\xe4\xb8\xad\xe6\x96\x87", 3) == "\xe4\xb8\xad\xe2\x80\xa6");
    // "中文" fits in 4
    REQUIRE(truncate_to_width("\xe4\xb8\xad\xe6\x96\x87", 4) == "\xe4\xb8\xad\xe6\x96\x87");
    // "中文" truncated to 2 → "中…"? No, width 2 means just "中" fits (width 2), no room for …
    // Actually "中" is width 2, fits exactly, but we need room for … (width 1)
    // So truncate to 2 → "中" (no ellipsis, exact fit)
    REQUIRE(truncate_to_width("\xe4\xb8\xad\xe6\x96\x87", 2) == "\xe4\xb8\xad");
    // "a中b" = 1+2+1=4, truncate to 2 → "a" (width 1) + no room for more... actually "a" + … = 1+1=2
    REQUIRE(truncate_to_width("a\xe4\xb8\xad" "b", 2) == "a\xe2\x80\xa6");
}

TEST_CASE("truncate_to_width edge cases", "[utf8]") {
    // max_width = 0 should return empty
    REQUIRE(truncate_to_width("hello", 0) == "");
    // Single char that fits
    REQUIRE(truncate_to_width("a", 1) == "a");
    // Single CJK char truncated to 1 → "…" (can't fit the CJK char, just ellipsis)
    REQUIRE(truncate_to_width("\xe4\xb8\xad", 1) == "\xe2\x80\xa6");
}
