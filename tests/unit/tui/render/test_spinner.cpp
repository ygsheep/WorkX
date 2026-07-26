/**
 * @file test_spinner.cpp
 * @brief Spinner 单元测试
 * @details 覆盖 start/stop/set_update_callback/is_running/elapsed_seconds
 *          以及回调触发、幂等性、析构安全
 *
 * @note Spinner 不通过 Terminal 写入输出（仅通过 UpdateCallback 通知），
 *       因此传入未 initialize() 的 Terminal 实例即可安全测试
 */

#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <thread>

#include "tui/core/terminal.h"
#include "tui/render/spinner.h"

using namespace tui;
using namespace std::chrono_literals;

namespace {

/// @brief 未 initialize() 的 Terminal，仅用于满足 Spinner 构造签名
/// @details Spinner 不调用 Terminal 的写入方法，未初始化的 Terminal 安全可用
Terminal null_terminal;

} // namespace

// ============================================================================
// 构造与初始状态
// ============================================================================

TEST_CASE("Spinner initial state is not running", "[spinner][init]") {
    Spinner spinner(&null_terminal);
    REQUIRE_FALSE(spinner.is_running());
    REQUIRE(spinner.elapsed_seconds() == 0);
}

// ============================================================================
// start / stop 生命周期
// ============================================================================

TEST_CASE("Spinner start sets running flag", "[spinner][lifecycle]") {
    Spinner spinner(&null_terminal);
    spinner.start("thinking");
    REQUIRE(spinner.is_running());
    spinner.stop();
}

TEST_CASE("Spinner stop clears running flag", "[spinner][lifecycle]") {
    Spinner spinner(&null_terminal);
    spinner.start("thinking");
    spinner.stop();
    REQUIRE_FALSE(spinner.is_running());
}

TEST_CASE("Spinner double start is idempotent", "[spinner][lifecycle]") {
    Spinner spinner(&null_terminal);
    spinner.start("first");
    spinner.start("second");  // no-op
    REQUIRE(spinner.is_running());
    spinner.stop();
}

TEST_CASE("Spinner stop without start is safe", "[spinner][lifecycle]") {
    Spinner spinner(&null_terminal);
    spinner.stop();
    REQUIRE_FALSE(spinner.is_running());
}

TEST_CASE("Spinner double stop is idempotent", "[spinner][lifecycle]") {
    Spinner spinner(&null_terminal);
    spinner.start("thinking");
    spinner.stop();
    spinner.stop();
    REQUIRE_FALSE(spinner.is_running());
}

TEST_CASE("Spinner restart after stop works", "[spinner][lifecycle]") {
    Spinner spinner(&null_terminal);
    spinner.start("first");
    spinner.stop();
    spinner.start("second");
    REQUIRE(spinner.is_running());
    spinner.stop();
    REQUIRE_FALSE(spinner.is_running());
}

// ============================================================================
// elapsed_seconds
// ============================================================================

TEST_CASE("Spinner elapsed_seconds is 0 when not running", "[spinner][elapsed]") {
    Spinner spinner(&null_terminal);
    REQUIRE(spinner.elapsed_seconds() == 0);

    spinner.start("thinking");
    spinner.stop();
    REQUIRE(spinner.elapsed_seconds() == 0);
}

TEST_CASE("Spinner elapsed_seconds increases over time", "[spinner][elapsed]") {
    Spinner spinner(&null_terminal);
    spinner.start("thinking");
    REQUIRE(spinner.elapsed_seconds() >= 0);
    std::this_thread::sleep_for(1100ms);
    REQUIRE(spinner.elapsed_seconds() >= 1);
    spinner.stop();
}

// ============================================================================
// update callback
// ============================================================================

TEST_CASE("Spinner update callback invoked while running", "[spinner][callback]") {
    Spinner spinner(&null_terminal);

    std::atomic<int> call_count{0};
    std::atomic<int32_t> last_elapsed{0};
    spinner.set_update_callback([&](int32_t elapsed) {
        call_count.fetch_add(1, std::memory_order_relaxed);
        last_elapsed.store(elapsed, std::memory_order_relaxed);
    });

    spinner.start("thinking");
    // FRAME_INTERVAL_MS = 100ms，等待 350ms 应至少触发 2 次回调
    std::this_thread::sleep_for(350ms);
    spinner.stop();

    REQUIRE(call_count.load() >= 2);
    REQUIRE(last_elapsed.load() >= 0);
}

TEST_CASE("Spinner update callback not invoked after stop", "[spinner][callback]") {
    Spinner spinner(&null_terminal);

    std::atomic<int> call_count{0};
    spinner.set_update_callback([&](int32_t) {
        call_count.fetch_add(1, std::memory_order_relaxed);
    });

    spinner.start("thinking");
    std::this_thread::sleep_for(150ms);
    spinner.stop();
    int count_after_stop = call_count.load();

    std::this_thread::sleep_for(200ms);
    REQUIRE(call_count.load() == count_after_stop);
}

TEST_CASE("Spinner callback observes increasing elapsed", "[spinner][callback]") {
    Spinner spinner(&null_terminal);

    std::atomic<int32_t> max_elapsed{0};
    std::atomic<int> calls{0};
    spinner.set_update_callback([&](int32_t elapsed) {
        int32_t prev = max_elapsed.load();
        while (elapsed > prev) {
            if (max_elapsed.compare_exchange_weak(prev, elapsed)) break;
        }
        calls.fetch_add(1, std::memory_order_relaxed);
    });

    spinner.start("thinking");
    std::this_thread::sleep_for(1200ms);
    spinner.stop();

    // 1.2s 内应至少观察到 1s 的 elapsed
    REQUIRE(max_elapsed.load() >= 1);
    REQUIRE(calls.load() >= 5);
}

TEST_CASE("Spinner callback replaced by second set_update_callback", "[spinner][callback]") {
    Spinner spinner(&null_terminal);

    std::atomic<int> first_calls{0};
    std::atomic<int> second_calls{0};

    spinner.set_update_callback([&](int32_t) {
        first_calls.fetch_add(1, std::memory_order_relaxed);
    });
    spinner.start("thinking");
    std::this_thread::sleep_for(150ms);
    spinner.stop();

    // 替换回调
    spinner.set_update_callback([&](int32_t) {
        second_calls.fetch_add(1, std::memory_order_relaxed);
    });
    int first_after_replace = first_calls.load();
    spinner.start("thinking");
    std::this_thread::sleep_for(150ms);
    spinner.stop();

    REQUIRE(second_calls.load() >= 1);
    REQUIRE(first_calls.load() == first_after_replace);  // 旧回调不再触发
}

// ============================================================================
// 析构安全
// ============================================================================

TEST_CASE("Spinner destructor stops thread safely", "[spinner][destructor]") {
    std::atomic<int> call_count{0};
    {
        Spinner spinner(&null_terminal);
        spinner.set_update_callback([&](int32_t) {
            call_count.fetch_add(1, std::memory_order_relaxed);
        });
        spinner.start("thinking");
        std::this_thread::sleep_for(150ms);
        REQUIRE(call_count.load() >= 1);
    }  // 析构调用 stop()
    int count_at_destroy = call_count.load();
    std::this_thread::sleep_for(200ms);
    REQUIRE(call_count.load() == count_at_destroy);  // 析构后回调不再触发
}
