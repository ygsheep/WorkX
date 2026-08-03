/**
 * @file test_provider_selector.cpp
 * @brief app/ui/provider_selector.h 单元测试
 * @details 验证 apply_provider_selection 的配置应用逻辑（/provider 命令核心）：
 *          - 写入 PROVIDER/REMOTE_URL/API_KEY/MODEL_NAME
 *          - 清除 CONTEXT_LENGTH（置 0，让 resolver 重新解析）
 *          - provider 为空时 no-op
 */

#include <catch2/catch_test_macros.hpp>

#include "app/config/app_config.h"
#include "app/ui/provider_selector.h"
#include "core/config/config_manager.h"

#include <string>

using namespace agent;

namespace {

struct TestCfg {
    ConfigManager& cfg;
    TestCfg() : cfg(ConfigManager::instance()) {
        cfg.clear();
        register_config_defaults(cfg);
    }
    ~TestCfg() { cfg.clear(); }
};

} // namespace

TEST_CASE("apply_provider_selection writes backend config", "[app][provider]") {
    TestCfg t;

    ProviderSelection sel;
    sel.provider = "deepseek";
    sel.remote_url = "https://api.deepseek.com";
    sel.api_path = "/v1/chat/completions";
    sel.model_name = "deepseek-v4-flash";
    sel.api_key = "sk-test-123";

    apply_provider_selection(t.cfg, sel);

    REQUIRE(t.cfg.get_or<std::string>(keys::PROVIDER, "") == "deepseek");
    REQUIRE(t.cfg.get_or<std::string>(keys::REMOTE_URL, "") == "https://api.deepseek.com");
    REQUIRE(t.cfg.get_or<std::string>(keys::MODEL_NAME, "") == "deepseek-v4-flash");
    REQUIRE(t.cfg.get_or<std::string>(keys::API_KEY, "") == "sk-test-123");
}

TEST_CASE("apply_provider_selection clears context_length for re-resolution", "[app][provider]") {
    TestCfg t;
    t.cfg.set(keys::CONTEXT_LENGTH, 1048576);

    ProviderSelection sel;
    sel.provider = "kimi";
    sel.model_name = "kimi-k3";
    apply_provider_selection(t.cfg, sel);

    REQUIRE(t.cfg.get_or<int>(keys::CONTEXT_LENGTH, 0) == 0);
}

TEST_CASE("apply_provider_selection empty provider is no-op", "[app][provider]") {
    TestCfg t;
    t.cfg.set(keys::PROVIDER, "glm");
    t.cfg.set(keys::CONTEXT_LENGTH, 1000000);

    ProviderSelection sel;  // provider 为空
    sel.remote_url = "https://should-not-write.example.com";
    apply_provider_selection(t.cfg, sel);

    REQUIRE(t.cfg.get_or<std::string>(keys::PROVIDER, "") == "glm");
    REQUIRE(t.cfg.get_or<std::string>(keys::REMOTE_URL, "") == "");
    REQUIRE(t.cfg.get_or<int>(keys::CONTEXT_LENGTH, 0) == 1000000);
}

TEST_CASE("apply_provider_selection skips empty optional fields", "[app][provider]") {
    TestCfg t;
    t.cfg.set(keys::REMOTE_URL, "https://old.example.com");
    t.cfg.set(keys::MODEL_NAME, "old-model");

    // custom 预设可能只有 URL，没有 model / api_key
    ProviderSelection sel;
    sel.provider = "openai-compatible";
    sel.remote_url = "https://custom.example.com/v1";
    sel.api_path = "/v1/chat/completions";
    // model_name 和 api_key 留空

    apply_provider_selection(t.cfg, sel);

    REQUIRE(t.cfg.get_or<std::string>(keys::REMOTE_URL, "") == "https://custom.example.com/v1");
    // 空 model 不覆盖已有值
    REQUIRE(t.cfg.get_or<std::string>(keys::MODEL_NAME, "") == "old-model");
    REQUIRE(t.cfg.get_or<std::string>(keys::API_KEY, "") == "");
}
