/**
 * @file test_remote_backend.cpp
 * @brief RemoteBackend 单元测试（H-1：DI 注入 IEventBus* 验证）
 * @details 验证 RemoteBackend 不再调用 EventBus::instance()，而是通过构造注入的
 *          IEventBus* 发布 BackendStatusEvent。使用 MockEventBus 做隔离测试。
 */

#include <catch2/catch_test_macros.hpp>

#include "agent/api/remote/remote_backend.h"
#include "agent/api/backend_factory.h"
#include "agent/api/backend_types.h"
#include "agent/message/types.h"  // BackendStatusEvent
#include "core/events/event_bus.h"  // H-A：验证未污染全局单例
#include "helpers/mock_event_bus.h"

using namespace agent;
using namespace agent::test;

namespace {

/// @brief 构造一个最小可用的 Remote BackendConfig
BackendConfig make_remote_config() {
    BackendConfig cfg;
    cfg.type = BackendConfig::Type::Remote;
    cfg.provider = ProviderType::OpenAI;
    cfg.base_url = "https://api.openai.com";
    cfg.api_key = "sk-test-key";
    cfg.model_name = "gpt-4o";
    cfg.timeout_ms = 5000;
    return cfg;
}

} // namespace

// ============================================================================
// H-1：RemoteBackend 构造接收 IEventBus*，不再调用单例
// ============================================================================

TEST_CASE("RemoteBackend initialize publishes Connected via injected bus", "[backend][remote][h-1]") {
    MockEventBus bus;
    RemoteBackend backend(&bus);

    SECTION("initialize 入队 Connected 事件") {
        auto result = backend.initialize(make_remote_config());
        REQUIRE(result.is_ok());
        REQUIRE(backend.is_ready());

        // publish_async 入队，未 process 前不计入 published_count
        REQUIRE(bus.async_queue_size() == 1);

        bus.process_async_events();
        REQUIRE(bus.published_count<BackendStatusEvent>() == 1);
        REQUIRE(bus.async_queue_size() == 0);
    }

    SECTION("shutdown 入队 Disconnected 事件") {
        backend.initialize(make_remote_config());
        bus.process_async_events();
        REQUIRE(bus.published_count<BackendStatusEvent>() == 1);

        backend.shutdown();
        REQUIRE(bus.async_queue_size() == 1);

        bus.process_async_events();
        REQUIRE(bus.published_count<BackendStatusEvent>() == 2);
    }

    SECTION("析构等价于 shutdown 发布 Disconnected") {
        {
            RemoteBackend b(&bus);
            b.initialize(make_remote_config());
            bus.process_async_events();
        } // 析构调用 shutdown
        bus.process_async_events();
        // 初始化 1 次 Connected + 析构 1 次 Disconnected
        REQUIRE(bus.published_count<BackendStatusEvent>() == 2);
    }
}

TEST_CASE("RemoteBackend event content correctness", "[backend][remote][h-1]") {
    MockEventBus bus;
    bus.set_dispatch_enabled(true);

    // 订阅并捕获事件内容
    BackendStatusEvent::Status captured_status = BackendStatusEvent::Disconnected;
    std::string captured_name;
    bus.subscribe<BackendStatusEvent>([&](const BackendStatusEvent& e) {
        captured_status = e.status;
        captured_name = e.backend_name;
    });

    RemoteBackend backend(&bus);

    SECTION("initialize 发布 Connected + backend_name=remote") {
        backend.initialize(make_remote_config());
        // publish_async -> process 后同步派发到订阅者
        bus.process_async_events();

        REQUIRE(captured_status == BackendStatusEvent::Connected);
        REQUIRE(captured_name == "remote");
    }

    SECTION("shutdown 发布 Disconnected + backend_name=remote") {
        backend.initialize(make_remote_config());
        bus.process_async_events();

        backend.shutdown();
        bus.process_async_events();

        REQUIRE(captured_status == BackendStatusEvent::Disconnected);
        REQUIRE(captured_name == "remote");
    }
}

TEST_CASE("RemoteBackend nullptr event_bus skips publishing - backward compat", "[backend][remote][h-1]") {
    // H-1：event_bus=nullptr 时不发布，保持向后兼容
    // H-A：扩展验证未污染全局单例（Closed AI Loop 防护）
    EventBus::instance().clear();

    RemoteBackend backend(nullptr);

    SECTION("initialize 不发布事件，不污染全局单例") {
        auto result = backend.initialize(make_remote_config());
        REQUIRE(result.is_ok());
        REQUIRE(backend.is_ready());

        // H-A：验证全局单例未被污染——这是 C-1 回归的检测点
        EventBus::instance().process_async_events();
        REQUIRE(EventBus::instance().async_queue_size() == 0);
    }

    SECTION("shutdown 不发布事件，不污染全局单例") {
        backend.initialize(make_remote_config());
        backend.shutdown();
        REQUIRE_FALSE(backend.is_ready());

        EventBus::instance().process_async_events();
        REQUIRE(EventBus::instance().async_queue_size() == 0);
    }
}

TEST_CASE("RemoteBackend initialize rejects invalid config", "[backend][remote][h-1]") {
    MockEventBus bus;
    RemoteBackend backend(&bus);

    SECTION("type 不匹配返回 InvalidInput") {
        BackendConfig cfg;
        cfg.type = BackendConfig::Type::Local;
        auto result = backend.initialize(cfg);
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == Error::Code::InvalidInput);
        REQUIRE(bus.async_queue_size() == 0);
    }

    SECTION("base_url 为空返回 InvalidInput") {
        BackendConfig cfg = make_remote_config();
        cfg.base_url.clear();
        auto result = backend.initialize(cfg);
        REQUIRE(result.is_err());
        REQUIRE(result.error().code == Error::Code::InvalidInput);
        REQUIRE(bus.async_queue_size() == 0);
    }
}

TEST_CASE("BackendFactory propagates event_bus to RemoteBackend", "[backend][factory][h-1]") {
    MockEventBus bus;

    SECTION("factory create 注入 event_bus 后端发布事件") {
        auto backend = BackendFactory::create(make_remote_config(), &bus);
        REQUIRE(backend != nullptr);
        REQUIRE(backend->name() == "remote");

        backend->initialize(make_remote_config());
        REQUIRE(bus.async_queue_size() == 1);
        bus.process_async_events();
        REQUIRE(bus.published_count<BackendStatusEvent>() == 1);
    }

    SECTION("factory create 默认 nullptr 不发布，不污染全局单例") {
        // H-A：扩展验证未污染全局单例——这是 C-1 回归的核心检测点
        EventBus::instance().clear();

        auto backend = BackendFactory::create(make_remote_config(), nullptr);
        REQUIRE(backend != nullptr);
        backend->initialize(make_remote_config());
        REQUIRE(backend->is_ready());

        // 验证 BackendStatusEvent 未发布到全局单例
        EventBus::instance().process_async_events();
        REQUIRE(EventBus::instance().async_queue_size() == 0);
    }
}
