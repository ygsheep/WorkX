/**
 * @file test_vt_input_decoder.cpp
 * @brief VtInputDecoder 单元测试：方向键/功能键/bracketed paste
 */

#include "tui/core/platform/vt_input_decoder.h"

#include <catch2/catch_test_macros.hpp>

namespace {

using Event = tui::VtInputDecoder::Event;

/// @brief 把一串码点喂给解码器，返回全部 Char 事件码（跳过中间态/标记）
std::vector<char32_t> feed_all(tui::VtInputDecoder& dec, const std::vector<char32_t>& cps) {
    std::vector<char32_t> out;
    for (char32_t cp : cps) {
        if (dec.feed(cp) == Event::Char) {
            out.push_back(dec.code());
        }
    }
    return out;
}

} // anonymous namespace

TEST_CASE("VtInputDecoder plain characters pass through", "[tui][vt_input]") {
    tui::VtInputDecoder dec;
    REQUIRE(dec.feed(U'a') == Event::Char);
    REQUIRE(dec.code() == U'a');
    REQUIRE(dec.feed(U'中') == Event::Char);
    REQUIRE(dec.code() == U'中');
    REQUIRE(dec.feed(U'\r') == Event::Char);  // 非粘贴模式：原样
    REQUIRE(dec.code() == U'\r');
}

TEST_CASE("VtInputDecoder arrow and function keys", "[tui][vt_input]") {
    tui::VtInputDecoder dec;
    SECTION("ESC[A-D arrows") {
        REQUIRE(dec.feed(0x1B) == Event::None);
        REQUIRE(dec.feed(U'[') == Event::None);
        REQUIRE(dec.feed(U'A') == Event::Char);
        REQUIRE(dec.code() == tui::VT_KEY_ARROW_UP);

        REQUIRE(feed_all(dec, {0x1B, U'[', U'B'}) == std::vector<char32_t>{tui::VT_KEY_ARROW_DOWN});
        REQUIRE(feed_all(dec, {0x1B, U'[', U'C'}) == std::vector<char32_t>{tui::VT_KEY_ARROW_RIGHT});
        REQUIRE(feed_all(dec, {0x1B, U'[', U'D'}) == std::vector<char32_t>{tui::VT_KEY_ARROW_LEFT});
    }
    SECTION("ESC[H/F and ESC[OH/OF Home/End") {
        REQUIRE(feed_all(dec, {0x1B, U'[', U'H'}) == std::vector<char32_t>{tui::VT_KEY_HOME});
        REQUIRE(feed_all(dec, {0x1B, U'[', U'F'}) == std::vector<char32_t>{tui::VT_KEY_END});
        REQUIRE(feed_all(dec, {0x1B, U'O', U'H'}) == std::vector<char32_t>{tui::VT_KEY_HOME});
        REQUIRE(feed_all(dec, {0x1B, U'O', U'F'}) == std::vector<char32_t>{tui::VT_KEY_END});
    }
    SECTION("ESC[1~/4~/7~/8~/5~ and ESC[3~") {
        REQUIRE(feed_all(dec, {0x1B, U'[', U'1', U'~'}) == std::vector<char32_t>{tui::VT_KEY_HOME});
        REQUIRE(feed_all(dec, {0x1B, U'[', U'4', U'~'}) == std::vector<char32_t>{tui::VT_KEY_END});
        REQUIRE(feed_all(dec, {0x1B, U'[', U'7', U'~'}) == std::vector<char32_t>{tui::VT_KEY_HOME});
        REQUIRE(feed_all(dec, {0x1B, U'[', U'8', U'~'}) == std::vector<char32_t>{tui::VT_KEY_HOME});
        REQUIRE(feed_all(dec, {0x1B, U'[', U'5', U'~'}) == std::vector<char32_t>{tui::VT_KEY_HOME});
        REQUIRE(feed_all(dec, {0x1B, U'[', U'3', U'~'}) == std::vector<char32_t>{tui::VT_KEY_DELETE});
    }
    SECTION("ESC[1;5C/D Ctrl+arrows") {
        REQUIRE(feed_all(dec, {0x1B, U'[', U'1', U';', U'5', U'C'}) ==
                std::vector<char32_t>{tui::VT_KEY_CTRL_ARROW_RIGHT});
        REQUIRE(feed_all(dec, {0x1B, U'[', U'1', U';', U'5', U'D'}) ==
                std::vector<char32_t>{tui::VT_KEY_CTRL_ARROW_LEFT});
    }
    SECTION("other modifiers fall back to plain arrow") {
        REQUIRE(feed_all(dec, {0x1B, U'[', U'1', U';', U'2', U'D'}) ==
                std::vector<char32_t>{tui::VT_KEY_ARROW_LEFT});
    }
}

TEST_CASE("VtInputDecoder bracketed paste", "[tui][vt_input]") {
    tui::VtInputDecoder dec;
    REQUIRE_FALSE(dec.paste_active());

    SECTION("paste markers toggle paste mode") {
        REQUIRE(dec.feed(0x1B) == Event::None);
        REQUIRE(dec.feed(U'[') == Event::None);
        REQUIRE(dec.feed(U'2') == Event::None);
        REQUIRE(dec.feed(U'0') == Event::None);
        REQUIRE(dec.feed(U'0') == Event::None);
        REQUIRE(dec.feed(U'~') == Event::PasteBegin);
        REQUIRE(dec.paste_active());

        REQUIRE(dec.feed(0x1B) == Event::None);
        REQUIRE(dec.feed(U'[') == Event::None);
        REQUIRE(dec.feed(U'2') == Event::None);
        REQUIRE(dec.feed(U'0') == Event::None);
        REQUIRE(dec.feed(U'1') == Event::None);
        REQUIRE(dec.feed(U'~') == Event::PasteEnd);
        REQUIRE_FALSE(dec.paste_active());
    }

    SECTION("newlines inside paste become \\n, tabs become spaces") {
        REQUIRE(feed_all(dec, {0x1B, U'[', U'2', U'0', U'0', U'~'}) == std::vector<char32_t>{});
        auto out = feed_all(dec, {U'h', U'\r', U'\n', U'i', U'\t', U'j'});
        REQUIRE(out == std::vector<char32_t>{U'h', U'\n', U'i', U' ', U'j'});
    }

    SECTION("bare LF inside paste stays a single newline") {
        REQUIRE(feed_all(dec, {0x1B, U'[', U'2', U'0', U'0', U'~'}) == std::vector<char32_t>{});
        auto out = feed_all(dec, {U'a', U'\n', U'b', U'\n', U'c'});
        REQUIRE(out == std::vector<char32_t>{U'a', U'\n', U'b', U'\n', U'c'});
    }

    SECTION("multiline paste keeps literal content") {
        REQUIRE(feed_all(dec, {0x1B, U'[', U'2', U'0', U'0', U'~'}) == std::vector<char32_t>{});
        auto out = feed_all(dec, {U'中', U'\r', U'\n', U'A', U'B', U'\r', U'\n', U'x'});
        REQUIRE(out == std::vector<char32_t>{U'中', U'\n', U'A', U'B', U'\n', U'x'});
        REQUIRE(feed_all(dec, {0x1B, U'[', U'2', U'0', U'1', U'~'}) == std::vector<char32_t>{});
    }

    SECTION("Enter after paste end is literal CR again") {
        REQUIRE(feed_all(dec, {0x1B, U'[', U'2', U'0', U'0', U'~'}) == std::vector<char32_t>{});
        REQUIRE(feed_all(dec, {0x1B, U'[', U'2', U'0', U'1', U'~'}) == std::vector<char32_t>{});
        REQUIRE(dec.feed(U'\r') == Event::Char);
        REQUIRE(dec.code() == U'\r');
    }
}

TEST_CASE("VtInputDecoder unknown sequences do not wedge state", "[tui][vt_input]") {
    tui::VtInputDecoder dec;
    SECTION("unknown CSI is discarded") {
        REQUIRE(dec.feed(0x1B) == Event::None);
        REQUIRE(dec.feed(U'[') == Event::None);
        REQUIRE(dec.feed(U'9') == Event::None);
        REQUIRE(dec.feed(U'9') == Event::None);
        REQUIRE(dec.feed(U'z') == Event::None);
        // 后续输入正常
        REQUIRE(dec.feed(U'a') == Event::Char);
        REQUIRE(dec.code() == U'a');
    }
    SECTION("ESC followed by non-sequence char yields ESC") {
        REQUIRE(dec.feed(0x1B) == Event::None);
        REQUIRE(dec.feed(U'x') == Event::Char);
        REQUIRE(dec.code() == 0x1B);
    }
    SECTION("unknown SS3 is discarded") {
        REQUIRE(dec.feed(0x1B) == Event::None);
        REQUIRE(dec.feed(U'O') == Event::None);
        REQUIRE(dec.feed(U'Z') == Event::None);
        REQUIRE(dec.feed(U'a') == Event::Char);
    }
}
