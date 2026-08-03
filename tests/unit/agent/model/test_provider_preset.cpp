/**
 * @file test_provider_preset.cpp
 * @brief ProviderPreset 查找和枚举测试
 */

#include <catch2/catch_test_macros.hpp>
#include "agent/model/provider_preset.h"

using namespace agent;

TEST_CASE("ProviderPreset find built-in presets", "[provider][preset]") {
    SECTION("deepseek preset exists and uses OpenAI type") {
        auto* preset = find_preset("deepseek");
        REQUIRE(preset != nullptr);
        REQUIRE(preset->type == ProviderType::OpenAI);
        REQUIRE(preset->name == "deepseek");
        REQUIRE_FALSE(preset->default_url.empty());
        REQUIRE_FALSE(preset->default_model.empty());
    }

    SECTION("glm preset exists") {
        auto* preset = find_preset("glm");
        REQUIRE(preset != nullptr);
        REQUIRE(preset->type == ProviderType::OpenAI);
        REQUIRE(preset->name == "glm");
        REQUIRE_FALSE(preset->default_url.empty());
        REQUIRE_FALSE(preset->default_model.empty());
    }

    SECTION("kimi preset exists") {
        auto* preset = find_preset("kimi");
        REQUIRE(preset != nullptr);
        REQUIRE(preset->type == ProviderType::OpenAI);
        REQUIRE(preset->name == "kimi");
        REQUIRE_FALSE(preset->default_url.empty());
        REQUIRE_FALSE(preset->default_model.empty());
    }

    SECTION("qwen preset exists") {
        auto* preset = find_preset("qwen");
        REQUIRE(preset != nullptr);
        REQUIRE(preset->type == ProviderType::OpenAI);
        REQUIRE(preset->name == "qwen");
        REQUIRE_FALSE(preset->default_url.empty());
        REQUIRE_FALSE(preset->default_model.empty());
    }

    SECTION("minimax preset exists") {
        auto* preset = find_preset("minimax");
        REQUIRE(preset != nullptr);
        REQUIRE(preset->type == ProviderType::OpenAI);
        REQUIRE(preset->name == "minimax");
        REQUIRE_FALSE(preset->default_url.empty());
        REQUIRE_FALSE(preset->default_model.empty());
    }

    SECTION("openai-compatible preset exists") {
        auto* preset = find_preset("openai-compatible");
        REQUIRE(preset != nullptr);
        REQUIRE(preset->type == ProviderType::OpenAI);
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

    bool has_deepseek = false, has_glm = false, has_kimi = false;
    for (auto n : names) {
        if (n == "deepseek") has_deepseek = true;
        if (n == "glm") has_glm = true;
        if (n == "kimi") has_kimi = true;
    }
    REQUIRE(has_deepseek);
    REQUIRE(has_glm);
    REQUIRE(has_kimi);
}
