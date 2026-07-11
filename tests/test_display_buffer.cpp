/**
 * @file test_display_buffer.cpp
 * @brief DisplayBuffer 单元测试
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include "tui/core/display_buffer.h"

using workx::DisplayBuffer;

TEST_CASE("DisplayBuffer basic single line", "[display_buffer]") {
    // height=4 → scroll_h=1, content fills the single-row scroll region
    DisplayBuffer db(100);
    db.set_width(80);
    db.set_height(4);

    SECTION("single line with newline") {
        db.feed("Hello\n");
        REQUIRE(db.row_count() == 1);
        auto snap = db.snapshot(1, 1);
        REQUIRE(snap.size() == 1);
        REQUIRE(snap[0] == "Hello");
    }

    SECTION("single line without newline") {
        db.feed("Hello");
        REQUIRE(db.row_count() == 0);
        auto snap = db.snapshot(1, 1);
        REQUIRE(snap.size() == 1);
        REQUIRE(snap[0].empty());
    }
}

TEST_CASE("DisplayBuffer multiple lines", "[display_buffer]") {
    // height=6 → scroll_h=3, exactly fills the scroll region
    DisplayBuffer db(100);
    db.set_width(80);
    db.set_height(6);

    db.feed("A\nB\nC\n");
    REQUIRE(db.row_count() == 3);
    auto snap = db.snapshot(1, 3);
    REQUIRE(snap.size() == 3);
    REQUIRE(snap[0] == "A");
    REQUIRE(snap[1] == "B");
    REQUIRE(snap[2] == "C");
}

TEST_CASE("DisplayBuffer SGR preservation", "[display_buffer]") {
    // height=4 → scroll_h=1
    DisplayBuffer db(100);
    db.set_width(80);
    db.set_height(4);

    SECTION("leading SGR preserved") {
        db.feed("\x1b[36mCyan\n");
        REQUIRE(db.row_count() == 1);
        auto snap = db.snapshot(1, 1);
        REQUIRE(snap[0] == "\x1b[36mCyan");
    }

    SECTION("set_color simulation: reset + set") {
        db.feed("\x1b[0m\x1b[36m");
        db.feed("Text\n");
        REQUIRE(db.row_count() == 1);
        auto snap = db.snapshot(1, 1);
        REQUIRE(snap[0] == "\x1b[0m\x1b[36mText");
    }

    SECTION("SGR reset mid-line") {
        db.feed("\x1b[36mAB\x1b[0mCD\n");
        REQUIRE(db.row_count() == 1);
        auto snap = db.snapshot(1, 1);
        REQUIRE(snap[0] == "\x1b[36mAB\x1b[0mCD");
    }
}

TEST_CASE("DisplayBuffer clear with 2J", "[display_buffer]") {
    DisplayBuffer db(100);
    db.set_width(80);
    db.set_height(5);

    db.feed("A\nB\n");
    REQUIRE(db.row_count() == 2);
    db.feed("\x1b[2J");
    REQUIRE(db.row_count() == 0);
    auto snap = db.snapshot(1, 3);
    for (const auto& s : snap) {
        REQUIRE(s.empty());
    }
}

TEST_CASE("DisplayBuffer auto-wrap", "[display_buffer]") {
    SECTION("exact width no wrap") {
        DisplayBuffer db(100);
        db.set_width(5);
        db.set_height(4);  // scroll_h=1
        db.feed("Hello");
        db.feed("\n");
        REQUIRE(db.row_count() == 1);
        auto snap = db.snapshot(1, 1);
        REQUIRE(snap[0] == "Hello");
    }

    SECTION("overflow wraps to next line") {
        DisplayBuffer db(100);
        db.set_width(5);
        db.set_height(5);  // scroll_h=2
        db.feed("HelloWorld\n");
        REQUIRE(db.row_count() == 2);
        auto snap = db.snapshot(1, 2);
        REQUIRE(snap[0] == "Hello");
        REQUIRE(snap[1] == "World");
    }
}

TEST_CASE("DisplayBuffer CJK width", "[display_buffer]") {
    DisplayBuffer db(100);
    db.set_width(4);
    db.set_height(5);  // scroll_h=2

    db.feed("\xe4\xb8\xad\xe6\x96\x87" "AB\n");
    REQUIRE(db.row_count() == 2);
    auto snap = db.snapshot(1, 2);
    REQUIRE(snap[0] == "\xe4\xb8\xad\xe6\x96\x87");
    REQUIRE(snap[1] == "AB");
}

TEST_CASE("DisplayBuffer snapshot out of range", "[display_buffer]") {
    // height=5 → scroll_h=2, only 1 line of content → bottom-aligned
    DisplayBuffer db(100);
    db.set_width(80);
    db.set_height(5);

    db.feed("A\n");
    REQUIRE(db.row_count() == 1);
    // physical = r + 1 - 2 = r - 1
    // r=1: physical=0 (empty), r=2: physical=1 ("A"), r=3..5: out of range
    auto snap = db.snapshot(1, 5);
    REQUIRE(snap.size() == 5);
    REQUIRE(snap[0].empty());       // above content
    REQUIRE(snap[1] == "A");        // bottom-aligned content
    REQUIRE(snap[2].empty());       // below scroll region
    REQUIRE(snap[3].empty());
    REQUIRE(snap[4].empty());
}

TEST_CASE("DisplayBuffer ring buffer overflow", "[display_buffer]") {
    // height=8 → scroll_h=5, capacity=3
    DisplayBuffer db(3);
    db.set_width(80);
    db.set_height(8);

    db.feed("A\nB\nC\nD\nE\n");
    REQUIRE(db.row_count() == 5);
    // physical = r + 5 - 5 = r, so rows 1..5 map to physical 1..5
    // capacity=3, total=5: physical 1,2 overwritten, 3,4,5 remain
    auto snap = db.snapshot(1, 5);
    REQUIRE(snap.size() == 5);
    REQUIRE(snap[0].empty());  // overwritten
    REQUIRE(snap[1].empty());  // overwritten
    REQUIRE(snap[2] == "C");
    REQUIRE(snap[3] == "D");
    REQUIRE(snap[4] == "E");
}

TEST_CASE("DisplayBuffer empty snapshot", "[display_buffer]") {
    DisplayBuffer db(100);
    db.set_width(80);
    db.set_height(24);

    REQUIRE(db.row_count() == 0);
    auto snap = db.snapshot(1, 3);
    REQUIRE(snap.size() == 3);
    for (const auto& s : snap) {
        REQUIRE(s.empty());
    }
}

TEST_CASE("DisplayBuffer scroll region mapping", "[display_buffer]") {
    // height=10 → scroll_h=7, 20 lines → scroll region full, shows last 7
    DisplayBuffer db(100);
    db.set_width(80);
    db.set_height(10);

    for (int i = 0; i < 20; i++) {
        db.feed("Line" + std::to_string(i) + "\n");
    }
    REQUIRE(db.row_count() == 20);

    int scroll_h = 10 - 3;
    auto snap = db.snapshot(1, scroll_h);
    REQUIRE(snap.size() == static_cast<size_t>(scroll_h));
    // physical = r + 20 - 7 = r + 13
    // r=1: physical=14 → Line13, r=7: physical=20 → Line19
    REQUIRE(snap[0] == "Line13");
    REQUIRE(snap[scroll_h - 1] == "Line19");
}

TEST_CASE("DisplayBuffer partial fill bottom alignment", "[display_buffer]") {
    // height=10 → scroll_h=7, only 3 lines → content at bottom 3 rows
    DisplayBuffer db(100);
    db.set_width(80);
    db.set_height(10);

    db.feed("A\nB\nC\n");
    REQUIRE(db.row_count() == 3);

    int scroll_h = 10 - 3;  // 7
    auto snap = db.snapshot(1, scroll_h);
    REQUIRE(snap.size() == static_cast<size_t>(scroll_h));
    // physical = r + 3 - 7 = r - 4
    // r=1..4: physical < 1 → empty (top of scroll region, no content)
    // r=5: physical=1 → "A"
    // r=6: physical=2 → "B"
    // r=7: physical=3 → "C"
    for (int i = 0; i < 4; i++) {
        REQUIRE(snap[i].empty());
    }
    REQUIRE(snap[4] == "A");
    REQUIRE(snap[5] == "B");
    REQUIRE(snap[6] == "C");
}
