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
#include "agent/api/backend_factory.h"  // H-C：BackendFactory::create
#include "agent/api/backend_types.h"    // H-C：BackendConfig
#include "agent/api/i_backend.h"
#include "agent/api/i_backend_admin.h"
#include "agent/api/i_completion_provider.h"
#include "agent/core/chat_session.h"
#include "agent/model/provider_preset.h"
#include "agent/tool/registry.h"
#include "core/events/event_bus.h"      // H-C：EventBus::instance()
#include "core/task/task_manager.h"     // H-C：TaskManager::instance()
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
    register_config_defaults(cfg);

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
    register_config_defaults(cfg);

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
    register_config_defaults(cfg);

    // M-1：显式注入 TaskManager / EventBus 单例（测试场景无 Mock 时复用单例）
    auto result = create_session(cfg, nullptr,
                                 TaskManager::instance(),
                                 EventBus::instance());

    REQUIRE(result.session == nullptr);
    REQUIRE(result.remote_url.empty());
    REQUIRE(result.model_name.empty());

    cfg.clear_for_test();
}

TEST_CASE("create_session resolves url from preset", "[factory][session]") {
    auto& cfg = ConfigManager::instance();
    cfg.clear_for_test();
    register_config_defaults(cfg);

    // 使用 lm-studio preset（默认 URL http://localhost:1234/v1）
    std::string provider_name = "lm-studio";
    const ProviderPreset* preset = find_preset(provider_name);
    REQUIRE(preset != nullptr);

    auto result = create_session(cfg, preset,
                                 TaskManager::instance(),
                                 EventBus::instance());

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

// ============================================================
// H-8: SessionResult 携带 backend_admin 字段
// ============================================================

TEST_CASE("SessionResult has backend_admin field (H-8)", "[factory][h8]") {
    // 编译期验证：SessionResult 包含 backend_admin 字段，类型为 IBackendAdmin*
    // （H-8：UI 层通过此字段调用 list_models/set_model_name，不再依赖 ChatSession::backend()）
    SessionResult result;
    REQUIRE(result.backend_admin == nullptr);  // 默认 nullptr
}

// ============================================================
// H-C: backend_admin 生命周期与 ChatSession 一致
// ============================================================

TEST_CASE("create_session no remote_url: backend_admin stays nullptr", "[factory][h-c]") {
    auto& cfg = ConfigManager::instance();
    cfg.clear_for_test();
    register_config_defaults(cfg);

    // M-1：显式注入 TaskManager / EventBus 单例
    auto result = create_session(cfg, nullptr,
                                 TaskManager::instance(),
                                 EventBus::instance());

    // 无 remote_url → session 未创建 → backend_admin 必须为 nullptr
    REQUIRE(result.session == nullptr);
    REQUIRE(result.backend_admin == nullptr);

    cfg.clear_for_test();
}

TEST_CASE("backend_admin shares lifetime with session (H-C)", "[factory][h-c]") {
    // H-C：验证 backend_admin 与 session 的生命周期绑定
    // 由于 create_session 需要 LM Studio 运行才能成功构造 session，
    // 此处通过手动构造 ChatSession + dynamic_cast 验证生命周期关系
    auto& cfg = ConfigManager::instance();
    cfg.clear_for_test();
    register_config_defaults(cfg);

    // 手动构造一个最小 backend（RemoteBackend，不 initialize）
    BackendConfig backend_config;
    backend_config.type = BackendConfig::Type::Remote;
    backend_config.provider = ProviderType::OpenAI;
    backend_config.base_url = "http://localhost:1234/v1";
    backend_config.model_name = "test-model";
    backend_config.api_key = "";
    backend_config.timeout_ms = 1000;

    auto backend = BackendFactory::create(backend_config, nullptr);
    REQUIRE(backend != nullptr);

    // C-2 修复路径：先构造 session，再通过 completion_provider() 获取 admin
    std::unique_ptr<ChatSession> session = std::make_unique<ChatSession>(
        std::move(backend),
        TaskManager::instance(),
        EventBus::instance(),
        cfg,
        1000, "test");

    IBackendAdmin* admin = nullptr;
    if (auto* provider = session->completion_provider()) {
        admin = dynamic_cast<IBackendAdmin*>(provider);
    }

    // admin 应非空（IBackend 同时实现 ICompletionProvider 和 IBackendAdmin）
    REQUIRE(admin != nullptr);

    // 验证生命周期绑定：session 存活期间 admin 有效
    REQUIRE(admin == dynamic_cast<IBackendAdmin*>(session->completion_provider()));

    // 析构 session 后 admin 悬垂（无法在 runtime 安全验证悬垂，但可验证
    // completion_provider() 与 admin 来自同一 session）
    session.reset();
    // admin 此时已悬垂，不进行任何操作（仅文档性验证）

    cfg.clear_for_test();
}

TEST_CASE("completion_provider exposes ICompletionProvider (H-C)", "[factory][h-c]") {
    // 验证 ChatSession::completion_provider() 返回的指针可正确 dynamic_cast
    // 到 IBackendAdmin（C-2 修复的核心契约）
    auto& cfg = ConfigManager::instance();
    cfg.clear_for_test();
    register_config_defaults(cfg);

    BackendConfig backend_config;
    backend_config.type = BackendConfig::Type::Remote;
    backend_config.provider = ProviderType::OpenAI;
    backend_config.base_url = "http://localhost:1234/v1";
    backend_config.model_name = "test";
    backend_config.api_key = "";
    backend_config.timeout_ms = 1000;

    auto backend = BackendFactory::create(backend_config, nullptr);
    REQUIRE(backend != nullptr);

    ChatSession session(std::move(backend),
                        TaskManager::instance(),
                        EventBus::instance(),
                        cfg,
                        1000, "test");

    // completion_provider() 应返回非空 ICompletionProvider*
    ICompletionProvider* provider = session.completion_provider();
    REQUIRE(provider != nullptr);

    // IBackend 同时实现 ICompletionProvider 和 IBackendAdmin，dynamic_cast 成功
    IBackendAdmin* admin = dynamic_cast<IBackendAdmin*>(provider);
    REQUIRE(admin != nullptr);

    cfg.clear_for_test();
}
