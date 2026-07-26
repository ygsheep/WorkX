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
