/**
 * @file test_cli_args.cpp
 * @brief cli_args 单元测试（M-2：DI 注入验证）
 * @details 验证 parse_cli_args 接收 IConfigManager& 后可独立测试，
 *          不依赖 ConfigManager::instance() 单例。
 */

#include <catch2/catch_test_macros.hpp>

#include "app/config/app_config.h"
#include "app/config/cli_args.h"
#include "core/config/config_manager.h"

using namespace agent;

TEST_CASE("parse_cli_args accepts IConfigManager injection (M-2)", "[cli_args][m2]") {
    auto& cfg = ConfigManager::instance();
    cfg.clear_for_test();
    register_config_defaults(cfg);

    // 模拟命令行参数
    char arg0[] = "workx";
    char arg1[] = "--simple-io";
    char arg2[] = "--no-color";
    char arg3[] = "--verbose";
    char arg4[] = "--model";
    char arg5[] = "gpt-4o";
    char* argv[] = {arg0, arg1, arg2, arg3, arg4, arg5};
    int argc = 6;

    parse_cli_args(cfg, argc, argv);

    REQUIRE(cfg.get_or<bool>(keys::SIMPLE_IO, false));
    REQUIRE(cfg.get_or<bool>(keys::NO_COLOR, false));
    REQUIRE(cfg.get_or<bool>(keys::VERBOSE, false));
    REQUIRE(cfg.get_or<std::string>(keys::MODEL_NAME, "") == "gpt-4o");

    cfg.clear_for_test();
}

TEST_CASE("parse_cli_args --timeout parses integer (M-2)", "[cli_args][m2]") {
    auto& cfg = ConfigManager::instance();
    cfg.clear_for_test();
    register_config_defaults(cfg);

    char arg0[] = "workx";
    char arg1[] = "--timeout";
    char arg2[] = "60000";
    char* argv[] = {arg0, arg1, arg2};
    int argc = 3;

    parse_cli_args(cfg, argc, argv);

    REQUIRE(cfg.get_or<int>(keys::TIMEOUT_MS, 0) == 60000);

    cfg.clear_for_test();
}
