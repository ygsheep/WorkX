/**
 * @file test_provider_preset.cpp
 * @brief ProviderPreset 查找和枚举测试
 */

#include <catch2/catch_test_macros.hpp>
#include "agent/model/provider_preset.h"

using namespace workx;

TEST_CASE("ProviderPreset find built-in presets", "[provider][preset]") {
    SECTION("openai preset exists") {
        auto* preset = find_preset("openai");
        REQUIRE(preset != nullptr);
        REQUIRE(preset->type == ProviderType::OpenAI);
        REQUIRE(preset->name == "openai");
        REQUIRE_FALSE(preset->default_url.empty());
        REQUIRE_FALSE(preset->default_model.empty());
    }

    SECTION("anthropic preset exists") {
        auto* preset = find_preset("anthropic");
        REQUIRE(preset != nullptr);
        REQUIRE(preset->type == ProviderType::Anthropic);
        REQUIRE(preset->name == "anthropic");
        REQUIRE_FALSE(preset->default_url.empty());
        REQUIRE_FALSE(preset->default_model.empty());
    }

    SECTION("deepseek preset exists and uses OpenAI type") {
        auto* preset = find_preset("deepseek");
        REQUIRE(preset != nullptr);
        REQUIRE(preset->type == ProviderType::OpenAI);
        REQUIRE(preset->name == "deepseek");
        REQUIRE_FALSE(preset->default_url.empty());
        REQUIRE_FALSE(preset->default_model.empty());
    }

    SECTION("groq preset exists") {
        auto* preset = find_preset("groq");
        REQUIRE(preset != nullptr);
        REQUIRE(preset->type == ProviderType::OpenAI);
    }

    SECTION("together preset exists") {
        auto* preset = find_preset("together");
        REQUIRE(preset != nullptr);
        REQUIRE(preset->type == ProviderType::OpenAI);
    }

    SECTION("openai-compatible preset exists") {
        auto* preset = find_preset("openai-compatible");
        REQUIRE(preset != nullptr);
        REQUIRE(preset->type == ProviderType::OpenAI);
    }

    SECTION("lm-studio preset exists") {
        auto* preset = find_preset("lm-studio");
        REQUIRE(preset != nullptr);
        REQUIRE(preset->type == ProviderType::OpenAI);
        REQUIRE(preset->name == "lm-studio");
        REQUIRE_FALSE(preset->default_url.empty());
    }
}

TEST_CASE("ProviderPreset unknown name returns nullptr", "[provider][preset]") {
    auto* preset = find_preset("nonexistent_provider_xyz");
    REQUIRE(preset == nullptr);

    auto* empty = find_preset("");
    REQUIRE(empty == nullptr);
}

TEST_CASE("ProviderPreset list all names", "[provider][preset]") {
    auto names = list_preset_names();
    REQUIRE(names.size() >= 5);

    bool has_openai = false, has_anthropic = false, has_deepseek = false;
    for (auto n : names) {
        if (n == "openai") has_openai = true;
        if (n == "anthropic") has_anthropic = true;
        if (n == "deepseek") has_deepseek = true;
    }
    REQUIRE(has_openai);
    REQUIRE(has_anthropic);
    REQUIRE(has_deepseek);
}
