/**
 * @file test_config_manager.cpp
 * @brief ConfigManager 单元测试
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "core/config/config_manager.h"

using namespace agent;

TEST_CASE("ConfigManager basic set/get", "[config]") {
    auto& cfg = ConfigManager::instance();
    cfg.clear_for_test();

    SECTION("set and get int") {
        auto result = cfg.set("test.int_val", 42);
        REQUIRE(result.isOk());
        auto val = cfg.get<int>("test.int_val");
        REQUIRE(val.isOk());
        REQUIRE(val.unwrap() == 42);
    }

    SECTION("set and get string") {
        cfg.set("test.str_val", std::string("hello"));
        auto val = cfg.get<std::string>("test.str_val");
        REQUIRE(val.isOk());
        REQUIRE(val.unwrap() == "hello");
    }

    SECTION("set and get bool") {
        cfg.set("test.bool_val", true);
        auto val = cfg.get<bool>("test.bool_val");
        REQUIRE(val.isOk());
        REQUIRE(val.unwrap() == true);
    }

    SECTION("set and get double") {
        cfg.set("test.double_val", 3.14);
        auto val = cfg.get<double>("test.double_val");
        REQUIRE(val.isOk());
        REQUIRE_THAT(val.unwrap(), Catch::Matchers::WithinAbs(3.14, 0.001));
    }

    cfg.clear_for_test();
}

TEST_CASE("ConfigManager get_or", "[config]") {
    auto& cfg = ConfigManager::instance();
    cfg.clear_for_test();

    SECTION("existing key returns value") {
        cfg.set("test.exists", 99);
        REQUIRE(cfg.get_or<int>("test.exists", 0) == 99);
    }

    SECTION("missing key returns default") {
        REQUIRE(cfg.get_or<int>("test.missing", 42) == 42);
    }

    cfg.clear_for_test();
}

TEST_CASE("ConfigManager validation", "[config]") {
    auto& cfg = ConfigManager::instance();
    cfg.clear_for_test();

    cfg.register_meta("test.validated", {
        .description = "Test validated key",
        .default_value = 10,
        .is_required = false,
        .validate_callback = [](const ConfigValue& v) -> Result<void, std::string> {
            if (std::holds_alternative<int>(v) && std::get<int>(v) < 0) {
                return Result<void, std::string>::err("must be >= 0");
            }
            return Result<void, std::string>::ok();
        },
        .change_callback = {}
    });

    SECTION("valid value accepted") {
        auto result = cfg.set("test.validated", 5);
        REQUIRE(result.isOk());
    }

    SECTION("invalid value rejected") {
        auto result = cfg.set("test.validated", -1);
        REQUIRE(result.isErr());
    }

    cfg.clear_for_test();
}

TEST_CASE("ConfigManager has and remove", "[config]") {
    auto& cfg = ConfigManager::instance();
    cfg.clear_for_test();

    cfg.set("test.temp", 1);
    REQUIRE(cfg.has("test.temp"));

    cfg.remove("test.temp");
    REQUIRE_FALSE(cfg.has("test.temp"));

    cfg.clear_for_test();
}

TEST_CASE("ConfigManager change callback", "[config]") {
    auto& cfg = ConfigManager::instance();
    cfg.clear_for_test();

    std::string changed_key;
    int old_val = 0;
    int new_val = 0;

    cfg.add_change_callback([&](const std::string& key,
                                 const ConfigValue& old_v,
                                 const ConfigValue& new_v) {
        changed_key = key;
        if (std::holds_alternative<int>(old_v)) old_val = std::get<int>(old_v);
        if (std::holds_alternative<int>(new_v)) new_val = std::get<int>(new_v);
    });

    cfg.set("test.cb", 10);
    cfg.set("test.cb", 20);

    REQUIRE(changed_key == "test.cb");
    REQUIRE(old_val == 10);
    REQUIRE(new_val == 20);

    cfg.clear_change_callbacks();
    cfg.clear_for_test();
}

TEST_CASE("ConfigScope", "[config]") {
    auto& cfg = ConfigManager::instance();
    cfg.clear_for_test();

    ConfigScope scope("myapp");
    scope.set("width", 800);

    REQUIRE(cfg.has("myapp.width"));
    REQUIRE(scope.get_or<int>("width", 0) == 800);
    REQUIRE(cfg.get_or<int>("myapp.width", 0) == 800);

    cfg.clear_for_test();
}

// C-3：ConfigScope DI 化测试
TEST_CASE("ConfigScope DI injection", "[config][di]") {
    auto& cfg = ConfigManager::instance();
    cfg.clear_for_test();

    SECTION("默认使用 ConfigManager::instance()") {
        ConfigScope scope("default");
        scope.set("key", 42);
        REQUIRE(cfg.get_or<int>("default.key", 0) == 42);
    }

    SECTION("注入自定义 IConfigManager（通过 MockConfigManager）") {
        // 使用 ConfigManager::instance() 作为注入目标验证 DI 路径
        // 真正的 Mock 测试在 test_mock_helpers.cpp 中
        ConfigScope scope("injected", cfg);
        scope.set("value", std::string("test"));
        REQUIRE(scope.get_or<std::string>("value", "") == "test");
        REQUIRE(cfg.get_or<std::string>("injected.value", "") == "test");
        REQUIRE(&scope.config_manager() == &cfg);
    }

    cfg.clear_for_test();
}

// C-2：ConfigSchema 测试
TEST_CASE("ConfigSchema validation", "[config][schema]") {
    auto& cfg = ConfigManager::instance();
    cfg.clear_for_test();

    SECTION("Int 范围校验") {
        cfg.register_schema({
            .key = "schema.int_val",
            .description = "Test int with range",
            .default_value = 50,
            .type = ConfigSchema::Type::Int,
            .int_range = std::make_pair<int64_t, int64_t>(0, 100)
        });

        REQUIRE(cfg.set("schema.int_val", 50).isOk());
        REQUIRE(cfg.set("schema.int_val", 150).isErr());  // 超范围
        REQUIRE(cfg.set("schema.int_val", -1).isErr());   // 超范围
        REQUIRE(cfg.set("schema.int_val", 0).isOk());     // 边界
        REQUIRE(cfg.set("schema.int_val", 100).isOk());   // 边界
    }

    SECTION("Enum 校验") {
        cfg.register_schema({
            .key = "schema.enum_val",
            .description = "Test enum",
            .default_value = std::string("a"),
            .type = ConfigSchema::Type::Enum,
            .enum_values = {"a", "b", "c"}
        });

        REQUIRE(cfg.set("schema.enum_val", std::string("a")).isOk());
        REQUIRE(cfg.set("schema.enum_val", std::string("b")).isOk());
        REQUIRE(cfg.set("schema.enum_val", std::string("d")).isErr());  // 非法值
    }

    SECTION("类型校验") {
        cfg.register_schema({
            .key = "schema.bool_val",
            .description = "Test bool",
            .default_value = false,
            .type = ConfigSchema::Type::Bool
        });

        REQUIRE(cfg.set("schema.bool_val", true).isOk());
        REQUIRE(cfg.set("schema.bool_val", 42).isErr());  // 类型不匹配
    }

    SECTION("get_schema / get_all_schemas") {
        cfg.register_schema({
            .key = "schema.lookup",
            .description = "Lookup test",
            .default_value = std::string("x"),
            .type = ConfigSchema::Type::String
        });

        auto result = cfg.get_schema("schema.lookup");
        REQUIRE(result.isOk());
        REQUIRE(result.unwrap().key == "schema.lookup");

        auto all = cfg.get_all_schemas();
        REQUIRE_FALSE(all.empty());
    }

    cfg.clear_for_test();
}

// C-4：环境变量加载测试
TEST_CASE("ConfigSchema load_from_env", "[config][env]") {
    auto& cfg = ConfigManager::instance();
    cfg.clear_for_test();

    SECTION("环境变量自动加载") {
        cfg.register_schema({
            .key = "env.test_str",
            .description = "Env string",
            .default_value = std::string("default"),
            .type = ConfigSchema::Type::String,
            .env_var = "WORKX_TEST_ENV_STR"
        });

        // 设置环境变量
        #ifdef _WIN32
        _putenv_s("WORKX_TEST_ENV_STR", "from_env");
        #else
        setenv("WORKX_TEST_ENV_STR", "from_env", 1);
        #endif

        cfg.load_from_env();
        REQUIRE(cfg.get_or<std::string>("env.test_str", "") == "from_env");

        // 清理环境变量
        #ifdef _WIN32
        _putenv_s("WORKX_TEST_ENV_STR", "");
        #else
        unsetenv("WORKX_TEST_ENV_STR");
        #endif
    }

    SECTION("Int 类型环境变量") {
        cfg.register_schema({
            .key = "env.test_int",
            .description = "Env int",
            .default_value = 0,
            .type = ConfigSchema::Type::Int,
            .int_range = std::make_pair<int64_t, int64_t>(0, 1000),
            .env_var = "WORKX_TEST_ENV_INT"
        });

        #ifdef _WIN32
        _putenv_s("WORKX_TEST_ENV_INT", "42");
        #else
        setenv("WORKX_TEST_ENV_INT", "42", 1);
        #endif

        cfg.load_from_env();
        REQUIRE(cfg.get_or<int>("env.test_int", 0) == 42);

        #ifdef _WIN32
        _putenv_s("WORKX_TEST_ENV_INT", "");
        #else
        unsetenv("WORKX_TEST_ENV_INT");
        #endif
    }

    cfg.clear_for_test();
}
