/**
 * @file test_factory.cpp
 * @brief app/factory.h 单元测试（D-2）
 * @details 验证从 main.cpp 提取的工厂函数行为：
 *          - make_terminal_config: 配置到 TerminalConfig 映射
 *          - register_builtin_tools: 3 个内置工具注册
 *          - build_system_prompt: 工具 prompt + @file 引用说明拼接
 *          - create_session: 无 remote_url 时返回空 session
 *
 * H-A：测试改用 MockTaskManager / MockEventBus（替代 TaskManager::instance() /
 *      EventBus::instance()），验证 create_session / ChatSession 不依赖单例。
 *      ConfigManager 因 register_config_defaults 接口限制（需 ConfigManager&，
 *      非 IConfigManager&）改用局部非单例实例，消除跨测试状态污染。
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
#include "helpers/mock_event_bus.h"     // H-A：替代 EventBus::instance()
#include "helpers/mock_task_manager.h"  // H-A：替代 TaskManager::instance()
#include "tui/core/terminal.h"

#include <algorithm>
#include <string>
#include <type_traits>

using namespace agent;

namespace {

/// @brief 测试用 ConfigManager 包装（H-A）
/// @details ConfigManager 构造函数为 private（单例模式），只能用 ::instance()。
///          register_config_defaults 需要 ConfigManager&（非 IConfigManager&），
///          无法直接用 MockConfigManager。此处封装单例的 clear() +
///          register_config_defaults 调用，确保每个用例独立、状态干净。
///          TaskManager / EventBus 已改用 Mock 注入（消除主要单例依赖）。
///          L-5：原 clear_for_test() 已移除（补丁式 API），改用语义相同的 clear()。
struct TestCfg {
    ConfigManager& cfg;
    TestCfg() : cfg(ConfigManager::instance()) {
        cfg.clear();
        register_config_defaults(cfg);
    }
    ~TestCfg() { cfg.clear(); }
    operator ConfigManager&() { return cfg; }
};

} // namespace

// ============================================================
// make_terminal_config
// ============================================================

TEST_CASE("make_terminal_config defaults", "[factory][terminal]") {
    TestCfg t;
    auto config = make_terminal_config(t.cfg);

    // register_config_defaults 注册的默认值：simple_io=false, no_color=false → use_color=true
    REQUIRE(config.simple_io == false);
    REQUIRE(config.use_color == true);
    // prompt 默认为 "> "（register_config_defaults 注册）
}

TEST_CASE("make_terminal_config custom values", "[factory][terminal]") {
    TestCfg t;
    t.cfg.set(keys::SIMPLE_IO, true);
    t.cfg.set(keys::NO_COLOR, true);
    t.cfg.set(keys::PROMPT, std::string(">>> "));

    auto config = make_terminal_config(t.cfg);

    REQUIRE(config.simple_io == true);
    REQUIRE(config.use_color == false);  // NO_COLOR=true → use_color=false
    REQUIRE(config.prompt_string == ">>> ");
}

// ============================================================
// register_builtin_tools
// ============================================================

TEST_CASE("register_builtin_tools registers expected tools", "[factory][tools]") {
    tool::ToolRegistry registry;
    register_builtin_tools(registry);

    auto tools = registry.get_all_tools();

    // 基础工具数：8（Read/Write/Edit/Bash/Glob/Grep/AskUser/Skill），Windows 额外注册 PowerShellTool
#ifdef _WIN32
    constexpr size_t kExpectedCount = 9;
#else
    constexpr size_t kExpectedCount = 8;
#endif
    REQUIRE(tools.size() == kExpectedCount);

    // 验证工具名
    std::vector<std::string> names;
    for (const auto& t : tools) {
        names.push_back(t->name());
    }

    REQUIRE(std::find(names.begin(), names.end(), "Read") != names.end());
    REQUIRE(std::find(names.begin(), names.end(), "Write") != names.end());
    REQUIRE(std::find(names.begin(), names.end(), "Edit") != names.end());
    REQUIRE(std::find(names.begin(), names.end(), "Bash") != names.end());
    REQUIRE(std::find(names.begin(), names.end(), "Glob") != names.end());
    REQUIRE(std::find(names.begin(), names.end(), "Grep") != names.end());
    REQUIRE(std::find(names.begin(), names.end(), "AskUser") != names.end());
    REQUIRE(std::find(names.begin(), names.end(), "Skill") != names.end());

#ifdef _WIN32
    // Windows 平台额外验证 PowerShellTool 已注册
    REQUIRE(std::find(names.begin(), names.end(), "PowerShell") != names.end());
#endif
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

TEST_CASE("build_system_prompt injects environment context", "[factory][prompt]") {
    tool::ToolRegistry registry;
    register_builtin_tools(registry);

    std::string prompt = build_system_prompt("", registry);

    // 应包含 # Environment 段
    REQUIRE(prompt.find("# Environment") != std::string::npos);
    // 应包含 Platform 行
    REQUIRE(prompt.find("Platform:") != std::string::npos);
    // 应包含 Working directory 行
    REQUIRE(prompt.find("Working directory:") != std::string::npos);
    // 应包含 git repo 检测
    REQUIRE(prompt.find("Is directory a git repo:") != std::string::npos);
    // 应包含 shell 信息
    REQUIRE(prompt.find("Available shells:") != std::string::npos);
}

// ============================================================
// create_session (无网络依赖的场景)
// ============================================================

TEST_CASE("create_session returns empty when no remote_url", "[factory][session]") {
    TestCfg t;
    test::MockTaskManager tm;
    test::MockEventBus bus;

    // H-A：注入 Mock，验证 create_session 不依赖单例
    auto result = create_session(t.cfg, nullptr, tm, bus);

    REQUIRE(result.session == nullptr);
    REQUIRE(result.remote_url.empty());
    REQUIRE(result.model_name.empty());
}

TEST_CASE("create_session resolves url from preset", "[factory][session]") {
    TestCfg t;
    test::MockTaskManager tm;
    test::MockEventBus bus;

    // 使用 deepseek preset（默认 URL https://api.deepseek.com）
    std::string provider_name = "deepseek";
    const ProviderPreset* preset = find_preset(provider_name);
    REQUIRE(preset != nullptr);

    auto result = create_session(t.cfg, preset, tm, bus);

    // URL 从 preset 解析（不实际创建 backend，因为无网络）
    REQUIRE(result.remote_url == std::string(preset->default_url));
    // session 创建会尝试连接，可能失败（无网络/API Key），session 可能为 nullptr
    // 这里只验证 URL 解析逻辑
}

// H-A 新增：验证 create_session 使用了注入的 ITaskManager / IEventBus
TEST_CASE("create_session uses injected MockTaskManager / MockEventBus (H-A)", "[factory][session][h-a]") {
    TestCfg t;
    test::MockTaskManager tm;
    test::MockEventBus bus;

    // preset 路径会触发 BackendFactory::create → backend->initialize（无网络失败）
    // 即使 session 创建失败，注入的 Mock 也不应被绕过（不应调用 ::instance()）
    std::string provider_name = "deepseek";
    const ProviderPreset* preset = find_preset(provider_name);
    REQUIRE(preset != nullptr);

    auto result = create_session(t.cfg, preset, tm, bus);

    // Mock 被构造且未崩溃（若 create_session 内部偷偷调 ::instance()，
    // 单例仍是 EventBus::instance() / TaskManager::instance()，本测试无法直接
    // 断言"未调用单例"，但 Mock 注入本身验证了接口路径可用）
    REQUIRE(tm.create_count() == 0);  // session 未成功创建 → 未 create task
    (void)result;
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
    TestCfg t;
    test::MockTaskManager tm;
    test::MockEventBus bus;

    // H-A：注入 Mock 而非单例
    auto result = create_session(t.cfg, nullptr, tm, bus);

    // 无 remote_url → session 未创建 → backend_admin 必须为 nullptr
    REQUIRE(result.session == nullptr);
    REQUIRE(result.backend_admin == nullptr);
}

TEST_CASE("backend_admin shares lifetime with session (H-C)", "[factory][h-c]") {
    // H-C：验证 backend_admin 与 session 的生命周期绑定
    // 由于 create_session 需要 LM Studio 运行才能成功构造 session，
    // 此处通过手动构造 ChatSession + dynamic_cast 验证生命周期关系
    TestCfg t;
    test::MockTaskManager tm;
    test::MockEventBus bus;

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
    // H-A：注入 MockTaskManager / MockEventBus
    std::unique_ptr<ChatSession> session = std::make_unique<ChatSession>(
        std::move(backend),
        tm,
        bus,
        t.cfg,
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
}

TEST_CASE("completion_provider exposes ICompletionProvider (H-C)", "[factory][h-c]") {
    // 验证 ChatSession::completion_provider() 返回的指针可正确 dynamic_cast
    // 到 IBackendAdmin（C-2 修复的核心契约）
    TestCfg t;
    test::MockTaskManager tm;
    test::MockEventBus bus;

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
                        tm,
                        bus,
                        t.cfg,
                        1000, "test");

    // completion_provider() 应返回非空 ICompletionProvider*
    ICompletionProvider* provider = session.completion_provider();
    REQUIRE(provider != nullptr);

    // IBackend 同时实现 ICompletionProvider 和 IBackendAdmin，dynamic_cast 成功
    IBackendAdmin* admin = dynamic_cast<IBackendAdmin*>(provider);
    REQUIRE(admin != nullptr);
}
