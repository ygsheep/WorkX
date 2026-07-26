/**
 * @file test_streaming_buffer.cpp
 * @brief StreamingBuffer 单元测试
 * @details 覆盖 start/stop/push 的状态管理、幂等性、析构安全
 *
 * @note StreamingBuffer 的 flush_now 在 buffer 非空时会调用
 *       Terminal::write_safe，而未 initialize() 的 Terminal m_platform 为 nullptr
 *       会崩溃。因此本测试限制为：
 *       1. buffer 始终为空时的 start/stop 行为
 *       2. push 非空内容但不 start（依赖析构跳过 flush_now）
 *       完整缓冲-刷新行为测试留待后续 Phase 抽象出 ITerminal 接口后补充。
 */

#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <thread>
#include <string>

#include "tui/core/terminal.h"
#include "tui/render/streaming_buffer.h"

using namespace tui;
using namespace std::chrono_literals;

namespace {

/// @brief 未 initialize() 的 Terminal，仅用于满足 StreamingBuffer 构造签名
/// @details buffer 为空时 flush_now 提前 return，不调用 write_safe
Terminal null_terminal;

} // namespace

// ============================================================================
// 构造与析构
// ============================================================================

TEST_CASE("StreamingBuffer constructs and destructs without start", "[streaming_buffer][init]") {
    StreamingBuffer buf(&null_terminal);
    // ~StreamingBuffer -> stop()，m_running=false 直接 return
}

TEST_CASE("StreamingBuffer destructor stops running thread", "[streaming_buffer][destructor]") {
    {
        StreamingBuffer buf(&null_terminal);
        buf.start();
        std::this_thread::sleep_for(50ms);
    }  // 析构调用 stop，buffer 为空，flush_now 提前 return
}

// ============================================================================
// start / stop 幂等性（buffer 始终为空）
// ============================================================================

TEST_CASE("StreamingBuffer start with empty buffer stops cleanly", "[streaming_buffer][lifecycle]") {
    StreamingBuffer buf(&null_terminal);
    buf.start();
    std::this_thread::sleep_for(50ms);
    buf.stop();
}

TEST_CASE("StreamingBuffer double start is idempotent", "[streaming_buffer][lifecycle]") {
    StreamingBuffer buf(&null_terminal);
    buf.start();
    buf.start();  // no-op
    std::this_thread::sleep_for(20ms);
    buf.stop();
}

TEST_CASE("StreamingBuffer stop without start is safe", "[streaming_buffer][lifecycle]") {
    StreamingBuffer buf(&null_terminal);
    buf.stop();  // m_running=false, 直接 return
}

TEST_CASE("StreamingBuffer double stop is idempotent", "[streaming_buffer][lifecycle]") {
    StreamingBuffer buf(&null_terminal);
    buf.start();
    buf.stop();
    buf.stop();  // no-op
}

TEST_CASE("StreamingBuffer restart after stop works", "[streaming_buffer][lifecycle]") {
    StreamingBuffer buf(&null_terminal);
    buf.start();
    buf.stop();
    buf.start();
    std::this_thread::sleep_for(20ms);
    buf.stop();
}

// ============================================================================
// push 行为
// ============================================================================

TEST_CASE("StreamingBuffer push empty string is no-op", "[streaming_buffer][push]") {
    StreamingBuffer buf(&null_terminal);
    buf.push("");
}

TEST_CASE("StreamingBuffer push without start accumulates in buffer safely", "[streaming_buffer][push]") {
    // push 只写入 m_buffer，不调用 Terminal
    // 未 start，析构时 stop 检测 m_running=false 直接 return，不调用 flush_now
    StreamingBuffer buf(&null_terminal);
    buf.push("hello");
    buf.push(" world");
    buf.push("{\"json\":\"data\"}");
}

TEST_CASE("StreamingBuffer push binary-like content does not crash", "[streaming_buffer][push]") {
    StreamingBuffer buf(&null_terminal);
    buf.push("\x1b[32m");
    buf.push("colored text");
    buf.push("\x1b[0m\n");
}

TEST_CASE("StreamingBuffer push large chunk without start is safe", "[streaming_buffer][push]") {
    StreamingBuffer buf(&null_terminal);
    std::string large(10000, 'x');
    buf.push(large);
}

// ============================================================================
// start + push 空字符串（buffer 始终为空）
// ============================================================================

TEST_CASE("StreamingBuffer start with empty pushes stops cleanly", "[streaming_buffer][push]") {
    // 启动刷新线程，但只 push 空字符串，buffer 始终为空
    StreamingBuffer buf(&null_terminal);
    buf.start();
    buf.push("");  // no-op
    buf.push("");
    std::this_thread::sleep_for(50ms);  // 刷新线程跑几轮，buffer 仍为空
    buf.stop();
}

// ============================================================================
// 综合场景
// ============================================================================

TEST_CASE("StreamingBuffer typical safe lifecycle", "[streaming_buffer][flow]") {
    // 模拟安全的使用流程：构造 → start → (空 push) → stop → 析构
    StreamingBuffer buf(&null_terminal);
    buf.start();
    std::this_thread::sleep_for(30ms);
    buf.push("");
    std::this_thread::sleep_for(30ms);
    buf.stop();
}
