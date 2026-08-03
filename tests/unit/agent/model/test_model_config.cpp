/**
 * @file test_model_config.cpp
 * @brief 模型能力表查找测试
 */

#include <catch2/catch_test_macros.hpp>
#include "agent/model/config.h"

using namespace agent;

TEST_CASE("ModelCapability exact match", "[model][config]") {
    SECTION("claude-sonnet-4-5 exact") {
        auto* cap = find_model_capability("claude-sonnet-4-5");
        REQUIRE(cap != nullptr);
        REQUIRE(cap->canonical_name == "claude-sonnet-4-5");
        REQUIRE(cap->context_window == 200'000);
        REQUIRE(cap->max_output_tokens == 16'384);
        REQUIRE(cap->supports_tools);
        REQUIRE(cap->supports_vision);
    }

    SECTION("case insensitive exact match") {
        auto* cap = find_model_capability("CLAUDE-SONNET-4-5");
        REQUIRE(cap != nullptr);
        REQUIRE(cap->canonical_name == "claude-sonnet-4-5");
    }

    SECTION("gpt-4o exact") {
        auto* cap = find_model_capability("gpt-4o");
        REQUIRE(cap != nullptr);
        REQUIRE(cap->context_window == 128'000);
        REQUIRE(cap->max_output_tokens == 16'384);
        REQUIRE(cap->supports_vision);
    }

    SECTION("deepseek-chat exact") {
        auto* cap = find_model_capability("deepseek-chat");
        REQUIRE(cap != nullptr);
        REQUIRE(cap->context_window == 64'000);
        REQUIRE(cap->supports_tools);
        REQUIRE_FALSE(cap->supports_vision);
    }

    SECTION("deepseek-v4-flash 1M context") {
        auto* cap = find_model_capability("deepseek-v4-flash");
        REQUIRE(cap != nullptr);
        REQUIRE(cap->context_window == 1'048'576);
        REQUIRE(cap->supports_tools);
    }

    SECTION("glm-5.2 1M context") {
        auto* cap = find_model_capability("glm-5.2");
        REQUIRE(cap != nullptr);
        REQUIRE(cap->context_window == 1'048'576);
    }

    SECTION("kimi-k3 1M context") {
        auto* cap = find_model_capability("kimi-k3");
        REQUIRE(cap != nullptr);
        REQUIRE(cap->context_window == 1'048'576);
        REQUIRE(cap->supports_tools);
        REQUIRE(cap->supports_vision);
    }

    SECTION("qwen3.7-max 1M context") {
        auto* cap = find_model_capability("qwen3.7-max");
        REQUIRE(cap != nullptr);
        REQUIRE(cap->context_window == 1'048'576);
    }

    SECTION("minimax-m3 1M context") {
        auto* cap = find_model_capability("minimax-m3");
        REQUIRE(cap != nullptr);
        REQUIRE(cap->context_window == 1'048'576);
    }
}

TEST_CASE("ModelCapability fuzzy match by substring", "[model][config]") {
    SECTION("dated claude model matches canonical name") {
        auto* cap = find_model_capability("claude-sonnet-4-5-20250929");
        REQUIRE(cap != nullptr);
        REQUIRE(cap->canonical_name == "claude-sonnet-4-5");
        REQUIRE(cap->context_window == 200'000);
    }

    SECTION("dated gpt-4o matches") {
        auto* cap = find_model_capability("gpt-4o-2024-11-20");
        REQUIRE(cap != nullptr);
        REQUIRE(cap->canonical_name == "gpt-4o");
    }

    SECTION("longest prefix wins when multiple match") {
        // "claude-sonnet-4" 和 "claude-sonnet-4-5" 都能匹配 "claude-sonnet-4-5-xxx"
        // 应该返回更长的 "claude-sonnet-4-5"
        auto* cap = find_model_capability("claude-sonnet-4-5-something");
        REQUIRE(cap != nullptr);
        REQUIRE(cap->canonical_name == "claude-sonnet-4-5");
    }
}

TEST_CASE("ModelCapability unknown model fallback", "[model][config]") {
    SECTION("unknown model returns nullptr") {
        auto* cap = find_model_capability("some-unknown-model-xyz");
        REQUIRE(cap == nullptr);
    }

    SECTION("empty string returns nullptr") {
        auto* cap = find_model_capability("");
        REQUIRE(cap == nullptr);
    }
}

TEST_CASE("get_context_window_for_model fallback", "[model][config]") {
    SECTION("known model returns configured window") {
        REQUIRE(get_context_window_for_model("claude-sonnet-4-5") == 200'000);
        REQUIRE(get_context_window_for_model("gpt-4o") == 128'000);
        REQUIRE(get_context_window_for_model("deepseek-v4-flash") == 1'048'576);
        REQUIRE(get_context_window_for_model("kimi-k3") == 1'048'576);
        REQUIRE(get_context_window_for_model("glm-5.2") == 1'048'576);
        REQUIRE(get_context_window_for_model("qwen3.7-max") == 1'048'576);
        REQUIRE(get_context_window_for_model("minimax-m3") == 1'048'576);
    }

    SECTION("unknown model returns default") {
        REQUIRE(get_context_window_for_model("unknown-model") == MODEL_CONTEXT_WINDOW_DEFAULT);
        REQUIRE(get_context_window_for_model("") == MODEL_CONTEXT_WINDOW_DEFAULT);
    }
}

TEST_CASE("get_max_output_tokens_for_model fallback", "[model][config]") {
    SECTION("known model returns configured output limit") {
        REQUIRE(get_max_output_tokens_for_model("claude-sonnet-4-5") == 16'384);
        REQUIRE(get_max_output_tokens_for_model("claude-opus-4-1") == 32'000);
    }

    SECTION("unknown model returns default") {
        REQUIRE(get_max_output_tokens_for_model("unknown-model") == MAX_OUTPUT_TOKENS_DEFAULT);
    }
}
