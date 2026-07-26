/**
 * @file test_event_bus.cpp
 * @brief EventBus 单元测试
 * @details 覆盖 subscribe/unsubscribe/publish/publish_async/process_async_events/clear
 *          以及 EventGuard RAII、异常安全、多订阅者、跨线程发布等场景
 */

#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <string>

#include "core/events/event_bus.h"

using namespace agent;
using namespace std::chrono_literals;

namespace {

/// @brief 测试用事件类型
struct TestEvent {
    int value = 0;
};

struct AnotherEvent {
    std::string msg;
};

/// @brief 每个测试前清理 EventBus 单例残留订阅
struct EventBusFixture {
    EventBusFixture() {
        EventBus::instance().clear();
    }
    ~EventBusFixture() {
        EventBus::instance().clear();
    }
};

} // namespace

// ============================================================================
// Basic subscribe & publish
// ============================================================================

TEST_CASE_METHOD(EventBusFixture, "EventBus single subscriber receives sync event", "[event_bus][basic]") {
    int received = 0;
    auto token = EventBus::instance().subscribe<TestEvent>(
        [&received](const TestEvent& e) { received = e.value; }
    );

    EventBus::instance().publish(TestEvent{.value = 42});

    REQUIRE(received == 42);
    EventBus::instance().unsubscribe<TestEvent>(token);
}

TEST_CASE_METHOD(EventBusFixture, "EventBus multiple subscribers all receive event", "[event_bus][basic]") {
    std::vector<int> received_a;
    std::vector<int> received_b;

    auto token_a = EventBus::instance().subscribe<TestEvent>(
        [&received_a](const TestEvent& e) { received_a.push_back(e.value); }
    );
    auto token_b = EventBus::instance().subscribe<TestEvent>(
        [&received_b](const TestEvent& e) { received_b.push_back(e.value); }
    );

    EventBus::instance().publish(TestEvent{.value = 1});
    EventBus::instance().publish(TestEvent{.value = 2});

    REQUIRE(received_a.size() == 2);
    REQUIRE(received_a[0] == 1);
    REQUIRE(received_a[1] == 2);
    REQUIRE(received_b.size() == 2);
    REQUIRE(received_b[0] == 1);
    REQUIRE(received_b[1] == 2);

    EventBus::instance().unsubscribe<TestEvent>(token_a);
    EventBus::instance().unsubscribe<TestEvent>(token_b);
}

TEST_CASE_METHOD(EventBusFixture, "EventBus different event types are isolated", "[event_bus][basic]") {
    int test_received = 0;
    int another_received = 0;

    auto t1 = EventBus::instance().subscribe<TestEvent>(
        [&test_received](const TestEvent& e) { test_received = e.value; }
    );
    auto t2 = EventBus::instance().subscribe<AnotherEvent>(
        [&another_received](const AnotherEvent&) { another_received++; }
    );

    EventBus::instance().publish(TestEvent{.value = 100});
    EventBus::instance().publish(AnotherEvent{.msg = "hello"});

    REQUIRE(test_received == 100);
    REQUIRE(another_received == 1);

    EventBus::instance().unsubscribe<TestEvent>(t1);
    EventBus::instance().unsubscribe<AnotherEvent>(t2);
}

// ============================================================================
// Unsubscribe
// ============================================================================

TEST_CASE_METHOD(EventBusFixture, "EventBus unsubscribe stops receiving events", "[event_bus][unsubscribe]") {
    int received = 0;
    auto token = EventBus::instance().subscribe<TestEvent>(
        [&received](const TestEvent& e) { received += e.value; }
    );

    EventBus::instance().publish(TestEvent{.value = 10});
    REQUIRE(received == 10);

    EventBus::instance().unsubscribe<TestEvent>(token);

    EventBus::instance().publish(TestEvent{.value = 100});
    REQUIRE(received == 10);  // no longer increments
}

TEST_CASE_METHOD(EventBusFixture, "EventBus unsubscribe invalid token does not affect others", "[event_bus][unsubscribe]") {
    int received = 0;
    auto token = EventBus::instance().subscribe<TestEvent>(
        [&received](const TestEvent& e) { received = e.value; }
    );

    EventToken invalid_token;  // default-constructed as invalid
    EventBus::instance().unsubscribe<TestEvent>(invalid_token);  // should not crash

    EventBus::instance().publish(TestEvent{.value = 5});
    REQUIRE(received == 5);

    EventBus::instance().unsubscribe<TestEvent>(token);
}

TEST_CASE_METHOD(EventBusFixture, "EventBus partial unsubscribe keeps others receiving", "[event_bus][unsubscribe]") {
    int a = 0, b = 0, c = 0;
    auto ta = EventBus::instance().subscribe<TestEvent>([&a](const TestEvent& e) { a = e.value; });
    auto tb = EventBus::instance().subscribe<TestEvent>([&b](const TestEvent& e) { b = e.value; });
    auto tc = EventBus::instance().subscribe<TestEvent>([&c](const TestEvent& e) { c = e.value; });

    EventBus::instance().unsubscribe<TestEvent>(tb);

    EventBus::instance().publish(TestEvent{.value = 7});

    REQUIRE(a == 7);
    REQUIRE(b == 0);  // unsubscribed
    REQUIRE(c == 7);

    EventBus::instance().unsubscribe<TestEvent>(ta);
    EventBus::instance().unsubscribe<TestEvent>(tc);
}

// ============================================================================
// Async publish
// ============================================================================

TEST_CASE_METHOD(EventBusFixture, "EventBus publish_async triggers callback via process_async_events", "[event_bus][async]") {
    int received = 0;
    auto token = EventBus::instance().subscribe<TestEvent>(
        [&received](const TestEvent& e) { received = e.value; }
    );

    EventBus::instance().publish_async(TestEvent{.value = 99});
    REQUIRE(received == 0);  // not consumed yet

    EventBus::instance().process_async_events();
    REQUIRE(received == 99);

    EventBus::instance().unsubscribe<TestEvent>(token);
}

TEST_CASE_METHOD(EventBusFixture, "EventBus multiple async events consumed in order", "[event_bus][async]") {
    std::vector<int> received_order;
    auto token = EventBus::instance().subscribe<TestEvent>(
        [&received_order](const TestEvent& e) { received_order.push_back(e.value); }
    );

    for (int i = 1; i <= 5; ++i) {
        EventBus::instance().publish_async(TestEvent{.value = i});
    }

    REQUIRE(received_order.empty());

    EventBus::instance().process_async_events();

    REQUIRE(received_order.size() == 5);
    REQUIRE(received_order[0] == 1);
    REQUIRE(received_order[4] == 5);

    EventBus::instance().unsubscribe<TestEvent>(token);
}

TEST_CASE_METHOD(EventBusFixture, "EventBus process_async_events clears queue", "[event_bus][async]") {
    int count = 0;
    auto token = EventBus::instance().subscribe<TestEvent>(
        [&count](const TestEvent&) { count++; }
    );

    EventBus::instance().publish_async(TestEvent{});
    EventBus::instance().publish_async(TestEvent{});
    EventBus::instance().process_async_events();
    REQUIRE(count == 2);

    // second process should have no new events
    EventBus::instance().process_async_events();
    REQUIRE(count == 2);

    EventBus::instance().unsubscribe<TestEvent>(token);
}

// ============================================================================
// clear & EventGuard RAII
// ============================================================================

TEST_CASE_METHOD(EventBusFixture, "EventBus clear removes all subscribers and async queue", "[event_bus][clear]") {
    int received = 0;
    [[maybe_unused]] auto token = EventBus::instance().subscribe<TestEvent>(
        [&received](const TestEvent& e) { received = e.value; }
    );

    EventBus::instance().publish_async(TestEvent{.value = 50});
    EventBus::instance().clear();

    // after clear, sync publish should not trigger (subscribers cleared)
    EventBus::instance().publish(TestEvent{.value = 999});
    REQUIRE(received == 0);

    // async queue should also be empty
    EventBus::instance().process_async_events();
    REQUIRE(received == 0);
}

TEST_CASE_METHOD(EventBusFixture, "EventGuard auto-unsubscribes on destruction", "[event_bus][guard]") {
    int received = 0;
    {
        auto guard = make_event_guard<TestEvent>(
            [&received](const TestEvent& e) { received = e.value; }
        );
        EventBus::instance().publish(TestEvent{.value = 10});
        REQUIRE(received == 10);
    }  // guard destroyed

    EventBus::instance().publish(TestEvent{.value = 100});
    REQUIRE(received == 10);  // no longer receives
}

TEST_CASE_METHOD(EventBusFixture, "EventGuard move invalidates source", "[event_bus][guard]") {
    int received = 0;
    std::unique_ptr<EventGuard<TestEvent>> guard_b;

    {
        auto guard_a = make_event_guard<TestEvent>(
            [&received](const TestEvent& e) { received = e.value; }
        );
        guard_b = std::make_unique<EventGuard<TestEvent>>(std::move(guard_a));
    }  // guard_a moved, does not unsubscribe

    EventBus::instance().publish(TestEvent{.value = 33});
    REQUIRE(received == 33);  // guard_b still holds subscription

    guard_b.reset();
    EventBus::instance().publish(TestEvent{.value = 999});
    REQUIRE(received == 33);  // guard_b destroyed, unsubscribed
}

// ============================================================================
// Exception safety
// ============================================================================

TEST_CASE_METHOD(EventBusFixture, "EventBus single callback throwing does not break others", "[event_bus][exception]") {
    int a = 0, c = 0;
    auto ta = EventBus::instance().subscribe<TestEvent>([&a](const TestEvent& e) {
        a = e.value;
        throw std::runtime_error("callback A failed");
    });
    auto tb = EventBus::instance().subscribe<TestEvent>([](const TestEvent&) {
        throw std::runtime_error("callback B failed");
    });
    auto tc = EventBus::instance().subscribe<TestEvent>([&c](const TestEvent& e) {
        c = e.value;
    });

    EventBus::instance().publish(TestEvent{.value = 7});

    REQUIRE(a == 7);
    REQUIRE(c == 7);  // C still called even though B threw

    EventBus::instance().unsubscribe<TestEvent>(ta);
    EventBus::instance().unsubscribe<TestEvent>(tb);
    EventBus::instance().unsubscribe<TestEvent>(tc);
}

TEST_CASE_METHOD(EventBusFixture, "EventBus sync publish does not hold lock during callback (no reentrancy deadlock)", "[event_bus][reentrancy]") {
    // Verifies T-1 fix: publish copies callback list and releases lock before invoking
    int received = 0;
    auto token = EventBus::instance().subscribe<TestEvent>(
        [&received](const TestEvent& e) {
            received = e.value;
            // publishing same-type event inside callback should not deadlock
            if (e.value < 3) {
                EventBus::instance().publish(TestEvent{.value = e.value + 1});
            }
        }
    );

    EventBus::instance().publish(TestEvent{.value = 1});
    // recursive publish should have driven received to 3
    REQUIRE(received == 3);

    EventBus::instance().unsubscribe<TestEvent>(token);
}

// ============================================================================
// Cross-thread publishing
// ============================================================================

TEST_CASE_METHOD(EventBusFixture, "EventBus concurrent publish_async is thread-safe", "[event_bus][thread]") {
    std::atomic<int> received{0};
    auto token = EventBus::instance().subscribe<TestEvent>(
        [&received](const TestEvent&) { received++; }
    );

    constexpr int THREAD_COUNT = 4;
    constexpr int EVENTS_PER_THREAD = 25;

    std::vector<std::thread> threads;
    for (int t = 0; t < THREAD_COUNT; ++t) {
        threads.emplace_back([]() {
            for (int i = 0; i < EVENTS_PER_THREAD; ++i) {
                EventBus::instance().publish_async(TestEvent{});
            }
        });
    }
    for (auto& th : threads) th.join();

    EventBus::instance().process_async_events();

    REQUIRE(received.load() == THREAD_COUNT * EVENTS_PER_THREAD);

    EventBus::instance().unsubscribe<TestEvent>(token);
}

TEST_CASE_METHOD(EventBusFixture, "EventBus cross-thread subscribe and publish", "[event_bus][thread]") {
    std::atomic<int> received{0};
    auto token = EventBus::instance().subscribe<TestEvent>(
        [&received](const TestEvent& e) { received.store(e.value); }
    );

    std::thread publisher([]() {
        EventBus::instance().publish(TestEvent{.value = 777});
    });

    publisher.join();
    REQUIRE(received.load() == 777);

    EventBus::instance().unsubscribe<TestEvent>(token);
}

// ============================================================================
// EventToken behavior
// ============================================================================

TEST_CASE("EventToken default-constructed is invalid", "[event_bus][token]") {
    EventToken token;
    REQUIRE_FALSE(token.is_valid());
    REQUIRE(token.get_id() == 0);
}

TEST_CASE("EventToken invalidate marks as invalid", "[event_bus][token]") {
    EventToken token(123);
    REQUIRE(token.is_valid());
    REQUIRE(token.get_id() == 123);

    token.invalidate();
    REQUIRE_FALSE(token.is_valid());
}

TEST_CASE("EventToken move transfers ownership", "[event_bus][token]") {
    EventToken token_a(42);
    EventToken token_b(std::move(token_a));

    REQUIRE(token_b.is_valid());
    REQUIRE(token_b.get_id() == 42);
    REQUIRE_FALSE(token_a.is_valid());  // moved-from
}
