/**
 * @file test_config_manager.cpp
 * @brief ConfigManager 单元测试（V2-1：ResultV2 迁移版）
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
        REQUIRE(result.is_ok());
        auto val = cfg.get<int>("test.int_val");
        REQUIRE(val.is_ok());
        REQUIRE(val.value() == 42);
    }

    SECTION("set and get string") {
        cfg.set("test.str_val", std::string("hello"));
        auto val = cfg.get<std::string>("test.str_val");
        REQUIRE(val.is_ok());
        REQUIRE(val.value() == "hello");
    }

    SECTION("set and get bool") {
        cfg.set("test.bool_val", true);
        auto val = cfg.get<bool>("test.bool_val");
        REQUIRE(val.is_ok());
        REQUIRE(val.value() == true);
    }

    SECTION("set and get double") {
        cfg.set("test.double_val", 3.14);
        auto val = cfg.get<double>("test.double_val");
        REQUIRE(val.is_ok());
        REQUIRE_THAT(val.value(), Catch::Matchers::WithinAbs(3.14, 0.001));
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

    cfg.register_meta("test.validated", ConfigMeta{
        .description = "Test validated key",
        .default_value = 10,
        .is_required = false,
        .validate_callback = [](const ConfigValue& v) -> ResultV2<void> {
            if (std::holds_alternative<int>(v) && std::get<int>(v) < 0) {
                return ResultV2<void>::err(
                    Error::Code::ConfigInvalid, "must be >= 0");
            }
            return ResultV2<void>::ok();
        },
        .change_callback = {}
    });

    SECTION("valid value accepted") {
        auto result = cfg.set("test.validated", 5);
        REQUIRE(result.is_ok());
    }

    SECTION("invalid value rejected with ConfigInvalid") {
        auto result = cfg.set("test.validated", -1);
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == Error::Code::ConfigInvalid);
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

    // H-4：ConfigScope 不再提供默认实参，需显式注入
    ConfigScope scope("myapp", cfg);
    scope.set("width", 800);

    REQUIRE(cfg.has("myapp.width"));
    REQUIRE(scope.get_or<int>("width", 0) == 800);
    REQUIRE(cfg.get_or<int>("myapp.width", 0) == 800);

    cfg.clear_for_test();
}

// C-3：ConfigScope DI 化测试（H-4：移除默认实参，所有调用方需显式注入）
TEST_CASE("ConfigScope DI injection", "[config][di]") {
    auto& cfg = ConfigManager::instance();
    cfg.clear_for_test();

    SECTION("显式注入 ConfigManager::instance()") {
        ConfigScope scope("default", ConfigManager::instance());
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
        cfg.register_schema(ConfigSchema{
            .key = "schema.int_val",
            .description = "Test int with range",
            .default_value = 50,
            .type = ConfigSchema::Type::Int,
            .int_range = std::make_pair<int64_t, int64_t>(0, 100)
        });

        REQUIRE(cfg.set("schema.int_val", 50).is_ok());
        REQUIRE(cfg.set("schema.int_val", 150).is_err());  // 超范围
        REQUIRE(cfg.set("schema.int_val", -1).is_err());   // 超范围
        REQUIRE(cfg.set("schema.int_val", 0).is_ok());     // 边界
        REQUIRE(cfg.set("schema.int_val", 100).is_ok());   // 边界
    }

    SECTION("Enum 校验") {
        cfg.register_schema(ConfigSchema{
            .key = "schema.enum_val",
            .description = "Test enum",
            .default_value = std::string("a"),
            .type = ConfigSchema::Type::Enum,
            .enum_values = {"a", "b", "c"}
        });

        REQUIRE(cfg.set("schema.enum_val", std::string("a")).is_ok());
        REQUIRE(cfg.set("schema.enum_val", std::string("b")).is_ok());
        REQUIRE(cfg.set("schema.enum_val", std::string("d")).is_err());  // 非法值
    }

    SECTION("类型校验") {
        cfg.register_schema(ConfigSchema{
            .key = "schema.bool_val",
            .description = "Test bool",
            .default_value = false,
            .type = ConfigSchema::Type::Bool
        });

        REQUIRE(cfg.set("schema.bool_val", true).is_ok());
        REQUIRE(cfg.set("schema.bool_val", 42).is_err());  // 类型不匹配
    }

    SECTION("get_schema / get_all_schemas") {
        cfg.register_schema(ConfigSchema{
            .key = "schema.lookup",
            .description = "Lookup test",
            .default_value = std::string("x"),
            .type = ConfigSchema::Type::String
        });

        auto result = cfg.get_schema("schema.lookup");
        REQUIRE(result.is_ok());
        REQUIRE(result.value().key == "schema.lookup");

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
        cfg.register_schema(ConfigSchema{
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
        cfg.register_schema(ConfigSchema{
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

// ============================================================
// V2-1 新增：Error::Code 断言测试
// ============================================================

TEST_CASE("ConfigManager V2-1 Error::Code", "[config][v2]") {
    auto& cfg = ConfigManager::instance();
    cfg.clear_for_test();

    SECTION("get 缺失键返回 ConfigMissing") {
        auto result = cfg.get<int>("nonexistent.key");
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == Error::Code::ConfigMissing);
        REQUIRE(result.error().context == "nonexistent.key");
    }

    SECTION("get 类型不匹配返回 ConfigInvalid") {
        cfg.set("type.mismatch", std::string("hello"));
        auto result = cfg.get<int>("type.mismatch");
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == Error::Code::ConfigInvalid);
        REQUIRE(result.error().context == "type.mismatch");
    }

    SECTION("set Schema 范围校验失败返回 ConfigInvalid") {
        cfg.register_schema(ConfigSchema{
            .key = "v2.range",
            .description = "Range test",
            .default_value = 50,
            .type = ConfigSchema::Type::Int,
            .int_range = std::make_pair<int64_t, int64_t>(0, 100)
        });

        auto result = cfg.set("v2.range", 200);
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == Error::Code::ConfigInvalid);
        REQUIRE(result.error().context == "v2.range");
    }

    SECTION("set Schema 枚举校验失败返回 ConfigInvalid") {
        cfg.register_schema(ConfigSchema{
            .key = "v2.enum",
            .description = "Enum test",
            .default_value = std::string("a"),
            .type = ConfigSchema::Type::Enum,
            .enum_values = {"a", "b", "c"}
        });

        auto result = cfg.set("v2.enum", std::string("z"));
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == Error::Code::ConfigInvalid);
        REQUIRE(result.error().context == "v2.enum");
    }

    SECTION("get_schema 缺失返回 ConfigMissing") {
        auto result = cfg.get_schema("nonexistent.schema");
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == Error::Code::ConfigMissing);
        REQUIRE(result.error().context == "nonexistent.schema");
    }

    SECTION("get_meta 缺失返回 ConfigMissing") {
        auto result = cfg.get_meta("nonexistent.meta");
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == Error::Code::ConfigMissing);
        REQUIRE(result.error().context == "nonexistent.meta");
    }

    SECTION("load_from_file 文件不存在返回 ResourceNotFound") {
        auto result = cfg.load_from_file("nonexistent_config_file.json");
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == Error::Code::ResourceNotFound);
    }

    cfg.clear_for_test();
}
