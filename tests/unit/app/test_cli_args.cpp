/**
 * @file test_cli_args.cpp
 * @brief cli_args 单元测试（M-2：DI 注入验证）
 * @details 验证 parse_cli_args 接收 IConfigManager& 后可独立测试，
 *          不依赖 ConfigManager::instance() 单例。
 *
 * H-B：改用 MockConfigManager 替代 ConfigManager::instance()。
 *      parse_cli_args 内部仅调用 cfg.set()，不依赖 register_schema，
 *      因此可完全用 MockConfigManager 验证 DI 解耦。
 */

#include <catch2/catch_test_macros.hpp>

#include "agent/config/app_config.h"
#include "app/config/cli_args.h"
#include "helpers/mock_config_manager.h"  // H-B：替代 ConfigManager::instance()

using namespace agent;

TEST_CASE("parse_cli_args accepts IConfigManager injection (M-2)", "[cli_args][m2][h-b]") {
    // H-B：注入 MockConfigManager，验证 parse_cli_args 不依赖单例
    test::MockConfigManager cfg;

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
}

TEST_CASE("parse_cli_args --timeout parses integer (M-2)", "[cli_args][m2][h-b]") {
    test::MockConfigManager cfg;

    char arg0[] = "workx";
    char arg1[] = "--timeout";
    char arg2[] = "60000";
    char* argv[] = {arg0, arg1, arg2};
    int argc = 3;

    parse_cli_args(cfg, argc, argv);

    REQUIRE(cfg.get_or<int>(keys::TIMEOUT_MS, 0) == 60000);
}

// #45：--bypass-permissions 设置 BypassPermissions 开关
TEST_CASE("parse_cli_args --bypass-permissions sets flag (#45)", "[cli_args][45]") {
    test::MockConfigManager cfg;

    char arg0[] = "workx";
    char arg1[] = "--bypass-permissions";
    char* argv[] = {arg0, arg1};
    int argc = 2;

    parse_cli_args(cfg, argc, argv);

    REQUIRE(cfg.get_or<bool>(keys::BYPASS_PERMISSIONS, false));
}

// #45：不传 --bypass-permissions 时默认 false（回归：行为与现状一致）
TEST_CASE("parse_cli_args defaults bypass_permissions false (#45)", "[cli_args][45]") {
    test::MockConfigManager cfg;

    char arg0[] = "workx";
    char arg1[] = "--verbose";
    char* argv[] = {arg0, arg1};
    int argc = 2;

    parse_cli_args(cfg, argc, argv);

    REQUIRE_FALSE(cfg.get_or<bool>(keys::BYPASS_PERMISSIONS, false));
}

// H-B 新增：验证 parse_cli_args 不修改未传入的键（Mock 隔离无副作用）
TEST_CASE("parse_cli_args does not touch unrelated keys (H-B)", "[cli_args][m2][h-b]") {
    test::MockConfigManager cfg;
    cfg.set(keys::SYSTEM_PROMPT, std::string("pre-existing"));

    char arg0[] = "workx";
    char arg1[] = "--simple-io";
    char* argv[] = {arg0, arg1};
    int argc = 2;

    parse_cli_args(cfg, argc, argv);

    // 未传 --system-prompt，原值应保留
    REQUIRE(cfg.get_or<std::string>(keys::SYSTEM_PROMPT, "") == "pre-existing");
    REQUIRE(cfg.get_or<bool>(keys::SIMPLE_IO, false));
}
