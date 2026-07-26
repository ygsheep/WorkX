/**
 * @file test_factory.cpp
 * @brief app/factory.h 单元测试（D-2）
 * @details 验证从 main.cpp 提取的工厂函数行为：
 *          - make_terminal_config: 配置到 TerminalConfig 映射
 *          - register_builtin_tools: 3 个内置工具注册
 *          - build_system_prompt: 工具 prompt + @file 引用说明拼接
 *          - create_session: 无 remote_url 时返回空 session
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include "app/factory.h"
#include "app/config/app_config.h"
#include "core/config/config_manager.h"
#include "agent/api/i_backend.h"
#include "agent/api/i_backend_admin.h"
#include "agent/api/i_completion_provider.h"
#include "agent/core/chat_session.h"
#include "agent/model/provider_preset.h"
#include "agent/tool/registry.h"
#include "tui/core/terminal.h"

#include <algorithm>
#include <string>
#include <type_traits>

using namespace agent;

// ============================================================
// make_terminal_config
// ============================================================

TEST_CASE("make_terminal_config defaults", "[factory][terminal]") {
    auto& cfg = ConfigManager::instance();
    cfg.clear_for_test();
    register_config_defaults();

    auto config = make_terminal_config(cfg);

    // register_config_defaults 注册的默认值：simple_io=false, no_color=false → use_color=true
    REQUIRE(config.simple_io == false);
    REQUIRE(config.use_color == true);
    // prompt 默认为 "> "（register_config_defaults 注册）

    cfg.clear_for_test();
}

TEST_CASE("make_terminal_config custom values", "[factory][terminal]") {
    auto& cfg = ConfigManager::instance();
    cfg.clear_for_test();
    register_config_defaults();

    cfg.set(keys::SIMPLE_IO, true);
    cfg.set(keys::NO_COLOR, true);
    cfg.set(keys::PROMPT, std::string(">>> "));

    auto config = make_terminal_config(cfg);

    REQUIRE(config.simple_io == true);
    REQUIRE(config.use_color == false);  // NO_COLOR=true → use_color=false
    REQUIRE(config.prompt_string == ">>> ");

    cfg.clear_for_test();
}

// ============================================================
// register_builtin_tools
// ============================================================

TEST_CASE("register_builtin_tools registers 3 tools", "[factory][tools]") {
    tool::ToolRegistry registry;
    register_builtin_tools(registry);

    auto tools = registry.get_all_tools();
    REQUIRE(tools.size() == 3);

    // 验证工具名（FileReadTool / FileWriteTool / FileEditTool）
    std::vector<std::string> names;
    for (const auto& t : tools) {
        names.push_back(t->name());
    }

    REQUIRE(std::find(names.begin(), names.end(), "Read") != names.end());
    REQUIRE(std::find(names.begin(), names.end(), "Write") != names.end());
    REQUIRE(std::find(names.begin(), names.end(), "Edit") != names.end());
}

// ============================================================
// build_system_prompt
// ============================================================

TEST_CASE("build_system_prompt empty user prompt", "[factory][prompt]") {
    tool::ToolRegistry registry;
    register_builtin_tools(registry);

    std::string prompt = build_system_prompt("", registry);

    // 应包含 @file 引用说明
    REQUIRE(prompt.find("<file path=") != std::string::npos);
    REQUIRE(prompt.find("@path") != std::string::npos);
    // 应包含至少一个工具 prompt
    REQUIRE(prompt.size() > 100);
}

TEST_CASE("build_system_prompt with user prompt", "[factory][prompt]") {
    tool::ToolRegistry registry;
    register_builtin_tools(registry);

    std::string prompt = build_system_prompt("You are a helpful assistant.", registry);

    // 应包含用户提示词
    REQUIRE(prompt.find("You are a helpful assistant.") == 0);
    // 应包含 @file 引用说明
    REQUIRE(prompt.find("<file path=") != std::string::npos);
}

TEST_CASE("build_system_prompt appends tool prompts", "[factory][prompt]") {
    tool::ToolRegistry registry;
    register_builtin_tools(registry);

    std::string prompt = build_system_prompt("", registry);

    // 每个工具都有 prompt()，应出现在结果中
    for (const auto& t : registry.get_all_tools()) {
        auto tool_prompt = t->prompt();
        if (!tool_prompt.empty()) {
            REQUIRE(prompt.find(tool_prompt) != std::string::npos);
        }
    }
}

// ============================================================
// create_session (无网络依赖的场景)
// ============================================================

TEST_CASE("create_session returns empty when no remote_url", "[factory][session]") {
    auto& cfg = ConfigManager::instance();
    cfg.clear_for_test();
    register_config_defaults();

    // 不设置 remote_url，也不设置 provider（无 preset）
    auto result = create_session(cfg, nullptr);

    REQUIRE(result.session == nullptr);
    REQUIRE(result.remote_url.empty());
    REQUIRE(result.model_name.empty());

    cfg.clear_for_test();
}

TEST_CASE("create_session resolves url from preset", "[factory][session]") {
    auto& cfg = ConfigManager::instance();
    cfg.clear_for_test();
    register_config_defaults();

    // 使用 lm-studio preset（默认 URL http://localhost:1234/v1）
    std::string provider_name = "lm-studio";
    const ProviderPreset* preset = find_preset(provider_name);
    REQUIRE(preset != nullptr);

    auto result = create_session(cfg, preset);

    // URL 从 preset 解析（不实际创建 backend，因为无网络）
    REQUIRE(result.remote_url == std::string(preset->default_url));
    // session 创建会尝试连接，可能失败（无 LM Studio 运行），session 可能为 nullptr
    // 这里只验证 URL 解析逻辑

    cfg.clear_for_test();
}

// ============================================================
// IBackendAdmin 接口隔离验证（D-3）
// ============================================================

TEST_CASE("IBackendAdmin is base of IBackend", "[factory][ibackend_admin]") {
    // 编译期验证：IBackend 继承 IBackendAdmin
    static_assert(std::is_base_of_v<IBackendAdmin, IBackend>,
                  "IBackend must inherit from IBackendAdmin");
    static_assert(std::is_base_of_v<ICompletionProvider, IBackend>,
                  "IBackend must inherit from ICompletionProvider");
}
