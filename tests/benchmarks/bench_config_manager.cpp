/**
 * @file bench_config_manager.cpp
 * @brief ConfigManager 性能基准（Q-2）
 * @details 测量配置读写吞吐，验证 Schema 校验不引入明显开销
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>

#include "core/config/config_manager.h"
#include "app/config/app_config.h"

#include <string>

using namespace agent;

TEST_CASE("ConfigManager get/set throughput", "[benchmark][config]") {
    // 确保配置已注册
    register_config_defaults();
    auto& cfg = ConfigManager::instance();

    BENCHMARK("get_or<string> 10k times") {
        std::string acc;
        for (int i = 0; i < 10'000; ++i) {
            acc += cfg.get_or<std::string>(keys::PROVIDER, "");
        }
        return acc;
    };

    BENCHMARK("get_or<int> 10k times") {
        int acc = 0;
        for (int i = 0; i < 10'000; ++i) {
            acc += cfg.get_or<int>(keys::CONTEXT_LENGTH, 0);
        }
        return acc;
    };

    BENCHMARK("set + get 10k times") {
        int acc = 0;
        for (int i = 0; i < 10'000; ++i) {
            cfg.set(keys::CONTEXT_LENGTH, i);
            acc += cfg.get_or<int>(keys::CONTEXT_LENGTH, 0);
        }
        return acc;
    };
}

TEST_CASE("ConfigManager has() lookup", "[benchmark][config]") {
    register_config_defaults();
    auto& cfg = ConfigManager::instance();

    BENCHMARK("has() 10k times (existing key)") {
        bool acc = false;
        for (int i = 0; i < 10'000; ++i) {
            acc = cfg.has(keys::PROVIDER);
        }
        return acc;
    };

    BENCHMARK("has() 10k times (missing key)") {
        bool acc = false;
        for (int i = 0; i < 10'000; ++i) {
            acc = cfg.has("nonexistent.key");
        }
        return acc;
    };
}
