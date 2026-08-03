/**
 * @file test_provider_selector.cpp
 * @brief app/ui/provider_form.h 单元测试
 * @details 验证多供应商配置的 ConfigManager 持久化与使用中切换逻辑：
 *          - load/save 往返（backend.providers JSON 数组）
 *          - apply_provider_switch 写入 PROVIDER/REMOTE_URL/API_KEY/MODEL_NAME
 *          - 清除 CONTEXT_LENGTH（置 0，让 resolver 重新解析）
 */

#include <catch2/catch_test_macros.hpp>

#include "app/config/app_config.h"
#include "app/ui/provider_form.h"
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

TEST_CASE("save/load provider configs roundtrip", "[app][provider]") {
    TestCfg t;

    std::vector<ProviderConfigEntry> providers = {
        {"deepseek", "DeepSeek", "https://api.deepseek.com", "deepseek-v4-flash", 1000000, "sk-1"},
        {"openai-compatible", "Custom URL", "http://localhost:1234/v1", "local-model", 0, ""}
    };
    save_provider_configs(t.cfg, providers);

    auto loaded = load_provider_configs(t.cfg);
    REQUIRE(loaded.size() == 2);
    REQUIRE(loaded[0].id == "deepseek");
    REQUIRE(loaded[0].name == "DeepSeek");
    REQUIRE(loaded[0].base_url == "https://api.deepseek.com");
    REQUIRE(loaded[0].model == "deepseek-v4-flash");
    REQUIRE(loaded[0].context_length == 1000000);
    REQUIRE(loaded[0].api_key == "sk-1");
    REQUIRE(loaded[1].id == "openai-compatible");
    REQUIRE(loaded[1].context_length == 0);
    REQUIRE(loaded[1].api_key.empty());
}

TEST_CASE("load provider configs missing key returns empty", "[app][provider]") {
    TestCfg t;
    REQUIRE(load_provider_configs(t.cfg).empty());
}

TEST_CASE("apply_provider_switch writes backend config", "[app][provider]") {
    TestCfg t;

    ProviderConfigEntry entry;
    entry.id = "deepseek";
    entry.name = "DeepSeek";
    entry.base_url = "https://api.deepseek.com";
    entry.model = "deepseek-v4-flash";
    entry.context_length = 1000000;
    entry.api_key = "sk-test-123";

    apply_provider_switch(t.cfg, entry);

    REQUIRE(t.cfg.get_or<std::string>(keys::PROVIDER, "") == "deepseek");
    REQUIRE(t.cfg.get_or<std::string>(keys::REMOTE_URL, "") == "https://api.deepseek.com");
    REQUIRE(t.cfg.get_or<std::string>(keys::MODEL_NAME, "") == "deepseek-v4-flash");
    REQUIRE(t.cfg.get_or<std::string>(keys::API_KEY, "") == "sk-test-123");
}

TEST_CASE("apply_provider_switch clears context_length for re-resolution", "[app][provider]") {
    TestCfg t;
    t.cfg.set(keys::CONTEXT_LENGTH, 1048576);

    ProviderConfigEntry entry;
    entry.id = "kimi";
    entry.name = "Kimi";
    entry.model = "kimi-k3";
    apply_provider_switch(t.cfg, entry);

    REQUIRE(t.cfg.get_or<int>(keys::CONTEXT_LENGTH, 0) == 0);
}

TEST_CASE("apply_provider_switch persists explicit entry context_length", "[app][provider]") {
    TestCfg t;
    t.cfg.set(keys::CONTEXT_LENGTH, 1048576);

    ProviderConfigEntry entry;
    entry.id = "qwen-vl";
    entry.name = "Qwen VL";
    entry.model = "Qwen2.5-VL-72B-Instruct";
    entry.context_length = 32000;
    apply_provider_switch(t.cfg, entry);

    // 显式配置的窗口写入标量（启动/热切换 resolver 的 user cfg 级生效）
    REQUIRE(t.cfg.get_or<int>(keys::CONTEXT_LENGTH, 0) == 32000);
}

TEST_CASE("apply_provider_switch falls back to name when id empty", "[app][provider]") {
    TestCfg t;

    ProviderConfigEntry entry;
    entry.name = "MyCustom";
    entry.base_url = "https://custom.example.com/v1";
    apply_provider_switch(t.cfg, entry);

    REQUIRE(t.cfg.get_or<std::string>(keys::PROVIDER, "") == "MyCustom");
    REQUIRE(t.cfg.get_or<std::string>(keys::REMOTE_URL, "") == "https://custom.example.com/v1");
}

TEST_CASE("apply_provider_switch clears stale keys from previous provider", "[app][provider]") {
    TestCfg t;
    t.cfg.set(keys::PROVIDER, std::string("deepseek"));
    t.cfg.set(keys::REMOTE_URL, std::string("https://api.deepseek.com"));
    t.cfg.set(keys::MODEL_NAME, std::string("deepseek-v4-flash"));
    t.cfg.set(keys::API_KEY, std::string("sk-stale"));

    // 切到 custom：全部字段留空（模拟全程跳过输入的 custom 预设）
    ProviderConfigEntry entry;
    entry.name = "LocalOnly";
    apply_provider_switch(t.cfg, entry);

    REQUIRE(t.cfg.get_or<std::string>(keys::PROVIDER, "") == "LocalOnly");
    REQUIRE_FALSE(t.cfg.has(keys::REMOTE_URL));   // 旧 URL 不得残留
    REQUIRE_FALSE(t.cfg.has(keys::MODEL_NAME));   // 旧模型名不得残留
    REQUIRE_FALSE(t.cfg.has(keys::API_KEY));      // 旧 API Key 不得残留
    REQUIRE(t.cfg.get_or<int>(keys::CONTEXT_LENGTH, 0) == 0);
}

TEST_CASE("load prefers providers array over legacy scalars", "[app][provider]") {
    TestCfg t;

    t.cfg.set(keys::PROVIDER, std::string("kimi"));  // 旧标量存在
    save_provider_configs(t.cfg, {
        {"glm", "GLM", "https://open.bigmodel.cn/api/paas/v4", "glm-4.6", 128000, "sk-glm"}
    });

    auto loaded = load_provider_configs(t.cfg);
    REQUIRE(loaded.size() == 1);
    REQUIRE(loaded[0].id == "glm");                  // 数组优先，不重复迁移
}

TEST_CASE("config manager persists json array via save_to_file", "[app][provider]") {
    TestCfg t;

    std::vector<ProviderConfigEntry> providers = {
        {"glm", "GLM", "https://open.bigmodel.cn/api/paas/v4", "glm-4.6", 128000, "sk-glm"}
    };
    save_provider_configs(t.cfg, providers);

    auto tmp = std::filesystem::temp_directory_path() / "workx_provider_test.json";
    auto save_result = t.cfg.save_to_file(tmp);
    REQUIRE(save_result.is_ok());

    // 重新加载验证数组往返
    t.cfg.clear();
    register_config_defaults(t.cfg);
    auto load_result = t.cfg.load_from_file(tmp);
    REQUIRE(load_result.is_ok());
    auto loaded = load_provider_configs(t.cfg);
    REQUIRE(loaded.size() == 1);
    REQUIRE(loaded[0].id == "glm");
    REQUIRE(loaded[0].api_key == "sk-glm");

    std::filesystem::remove(tmp);
}
