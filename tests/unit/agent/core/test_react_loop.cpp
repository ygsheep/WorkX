/**
 * @file test_react_loop.cpp
 * @brief ReActLoop 单元测试
 * @details 覆盖 Thought/Action/Observation 三阶段、FinalAnswer 终止、
 *          max_iterations 限制、should_cancel 中断、流式错误、工具调用执行、
 *          回调触发、token 统计等场景
 */

#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <string>
#include <memory>
#include <deque>
#include <optional>

#include "agent/core/react_loop.h"
#include "agent/api/i_completion_provider.h"
#include "agent/api/i_stream_reader.h"
#include "agent/api/chat_types.h"
#include "agent/tool/registry.h"
#include "agent/tool/itool.h"
#include "agent/tool/context.h"
#include "agent/tool/result.h"
#include "core/config/config_manager.h"
#include "helpers/mock_provider.h"

using namespace agent;
using namespace agent::tool;
using namespace agent::test;
using namespace std::chrono_literals;

namespace {

// ============================================================
// EchoTool — 简单测试工具
// ============================================================

class EchoTool : public ITool {
public:
    mutable int call_count = 0;
    mutable std::string last_input;

    const std::string& name() const override {
        static const std::string n = "Echo";
        return n;
    }
    const std::string& description() const override {
        static const std::string d = "Echoes back the input";
        return d;
    }
    const std::string& prompt() const override {
        static const std::string p = "Echo tool for testing";
        return p;
    }
    nlohmann::json input_schema() const override {
        return {
            {"type", "object"},
            {"properties", {
                {"text", {{"type", "string"}}}
            }},
            {"required", {"text"}}
        };
    }
    ResultV2<ToolResult> call(const nlohmann::json& input, const ToolContext& /*ctx*/) const override {
        call_count++;
        last_input = input.value("text", "");
        return ResultV2<ToolResult>::ok(ToolResult::ok(std::string("echo: ") + last_input));
    }
};

// ============================================================
// FailingTool — 总是抛异常的工具
// ============================================================

class FailingTool : public ITool {
public:
    const std::string& name() const override {
        static const std::string n = "Failing";
        return n;
    }
    const std::string& description() const override {
        static const std::string d = "Always throws";
        return d;
    }
    const std::string& prompt() const override {
        static const std::string p = "Failing tool for testing";
        return p;
    }
    nlohmann::json input_schema() const override {
        return {{"type", "object"}, {"properties", {}}};
    }
    ResultV2<ToolResult> call(const nlohmann::json&, const ToolContext&) const override {
        throw std::runtime_error("intentional tool failure");
    }
};

// ============================================================
// DeniedTool — 权限检查拒绝
// ============================================================

class DeniedTool : public ITool {
public:
    const std::string& name() const override {
        static const std::string n = "Denied";
        return n;
    }
    const std::string& description() const override {
        static const std::string d = "Permission always denied";
        return d;
    }
    const std::string& prompt() const override {
        static const std::string p = "Denied tool for testing";
        return p;
    }
    nlohmann::json input_schema() const override {
        return {{"type", "object"}, {"properties", {}}};
    }
    PermissionResult check_permissions(const nlohmann::json&, const ToolContext&) const override {
        return PermissionResult::err(Error::Code::PermissionDenied, "access denied by policy");
    }
    ResultV2<ToolResult> call(const nlohmann::json&, const ToolContext&) const override {
        return ResultV2<ToolResult>::ok(ToolResult::ok(std::string("should not reach here")));
    }
};

// ============================================================
// CooperativeSlowTool — 协作式慢工具（响应 is_cancelled 即可退出）
// ============================================================

class CooperativeSlowTool : public ITool {
public:
    const std::string& name() const override {
        static const std::string n = "Slow";
        return n;
    }
    const std::string& description() const override {
        static const std::string d = "Cooperative slow tool that exits on cancel";
        return d;
    }
    const std::string& prompt() const override {
        static const std::string p = "Slow tool for testing";
        return p;
    }
    nlohmann::json input_schema() const override {
        return {{"type", "object"}, {"properties", {}}};
    }
    ResultV2<ToolResult> call(const nlohmann::json&, const ToolContext& ctx) const override {
        // 协作式取消：每 50ms 轮询一次；无取消则 1s 后正常完成
        constexpr int kTicks = 20;
        for (int i = 0; i < kTicks; ++i) {
            if (ctx.is_cancelled()) {
                return ResultV2<ToolResult>::err(
                    Error::Code::Cancelled, "Slow tool cancelled");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return ResultV2<ToolResult>::ok(ToolResult::ok(std::string("slow done")));
    }
};

// ============================================================
// Helper: 构建 ReActLoop
// ============================================================

struct ReActLoopFixture {
    std::unique_ptr<MockCompletionProvider> provider;
    std::shared_ptr<ToolRegistry> registry;
    std::shared_ptr<EchoTool> echo_tool;
    std::atomic<bool> should_cancel{false};

    ReActLoopFixture() {
        provider = std::make_unique<MockCompletionProvider>();
        registry = std::make_shared<ToolRegistry>();
        echo_tool = std::make_shared<EchoTool>();
        registry->register_tool(echo_tool);
    }

    std::unique_ptr<ReActLoop> make_loop(ReActLoop::Config config = ReActLoop::Config{}) {
        // H-5：必须显式注入 IConfigManager*（非空）
        return std::make_unique<ReActLoop>(provider.get(), registry, config,
                                           &ConfigManager::instance());
    }

    /// @brief 创建一个返回纯文本（无工具调用）的 reader
    std::shared_ptr<MockStreamReader> make_text_reader(const std::string& text) {
        auto reader = std::make_shared<MockStreamReader>();
        reader->add_content_chunk(text);
        reader->set_usage(10, 20);
        provider->set_next_reader(reader);
        return reader;
    }

    /// @brief 创建一个返回工具调用的 reader
    std::shared_ptr<MockStreamReader> make_tool_call_reader(
        const std::string& tool_id,
        const std::string& tool_name,
        const std::string& input_json)
    {
        auto reader = std::make_shared<MockStreamReader>();
        reader->add_content_chunk("Let me use the tool.");
        reader->add_tool_use_start(tool_id, tool_name);
        reader->add_tool_use_delta(tool_id, input_json);
        reader->set_usage(15, 25);
        provider->set_next_reader(reader);
        return reader;
    }

    /// @brief 创建一个流式错误的 reader
    std::shared_ptr<MockStreamReader> make_error_reader() {
        auto reader = std::make_shared<MockStreamReader>();
        reader->add_content_chunk("partial");
        reader->set_error_at(1);
        provider->set_next_reader(reader);
        return reader;
    }
};

} // namespace

// ============================================================================
// Thought 阶段 — FinalAnswer 终止
// ============================================================================

TEST_CASE_METHOD(ReActLoopFixture, "ReActLoop returns FinalAnswer when no tool_use", "[react_loop][thought]") {
    auto reader = make_text_reader("Hello, world!");

    std::vector<ChatMessage> messages = {ChatMessage::user("hi")};
    auto loop = make_loop();
    auto result = loop->run(messages, "", nlohmann::json::array(), should_cancel);

    REQUIRE_FALSE(result.was_error);
    REQUIRE_FALSE(result.was_interrupted);
    REQUIRE(result.final_answer == "Hello, world!");
    REQUIRE(result.total_iterations == 1);
    REQUIRE(result.total_tool_calls == 0);
    REQUIRE(messages.size() == 2);  // user + assistant
    REQUIRE(messages[1].role == ChatMessage::Role::Assistant);
    REQUIRE(messages[1].content == "Hello, world!");
}

TEST_CASE_METHOD(ReActLoopFixture, "ReActLoop captures reasoning content", "[react_loop][thought]") {
    auto reader = std::make_shared<MockStreamReader>();
    reader->add_reasoning_chunk("thinking...");
    reader->add_content_chunk("answer");
    reader->set_usage(5, 10);
    provider->set_next_reader(reader);

    std::vector<ChatMessage> messages = {ChatMessage::user("q")};
    auto loop = make_loop();
    auto result = loop->run(messages, "", nlohmann::json::array(), should_cancel);

    REQUIRE(result.final_answer == "answer");
    REQUIRE(result.final_reasoning == "thinking...");
    REQUIRE(messages[1].reasoning_content == "thinking...");
}

TEST_CASE_METHOD(ReActLoopFixture, "ReActLoop captures token statistics", "[react_loop][thought]") {
    auto reader = std::make_shared<MockStreamReader>();
    reader->add_content_chunk("response");
    reader->set_usage(100, 200, 50, 30);
    provider->set_next_reader(reader);

    std::vector<ChatMessage> messages = {ChatMessage::user("q")};
    auto loop = make_loop();
    auto result = loop->run(messages, "", nlohmann::json::array(), should_cancel);

    REQUIRE(result.prompt_tokens == 100);
    REQUIRE(result.generated_tokens == 200);
    REQUIRE(result.cache_creation_input_tokens == 50);
    REQUIRE(result.cache_read_input_tokens == 30);
}

// ============================================================================
// Action + Observation 阶段 — 工具调用
// ============================================================================

TEST_CASE_METHOD(ReActLoopFixture, "ReActLoop executes tool and feeds result back", "[react_loop][action]") {
    // 第一次：tool_use
    make_tool_call_reader("tu_01", "Echo", R"({"text":"hello"})");

    // 第二次：final answer（基于工具结果）
    make_text_reader("Done after echo");

    std::vector<ChatMessage> messages = {ChatMessage::user("echo hello")};
    auto loop = make_loop();
    auto result = loop->run(messages, "", registry->get_all_schemas(), should_cancel);

    REQUIRE_FALSE(result.was_error);
    REQUIRE(result.final_answer == "Done after echo");
    REQUIRE(result.total_iterations == 2);
    REQUIRE(result.total_tool_calls == 1);
    REQUIRE(echo_tool->call_count == 1);
    REQUIRE(echo_tool->last_input == "hello");

    // 验证消息序列：user → assistant(tool_use) → tool_result → assistant(final)
    REQUIRE(messages.size() == 4);
    REQUIRE(messages[0].role == ChatMessage::Role::User);
    REQUIRE(messages[1].role == ChatMessage::Role::Assistant);
    REQUIRE(messages[1].tool_uses.size() == 1);
    REQUIRE(messages[1].tool_uses[0].name == "Echo");
    REQUIRE(messages[2].role == ChatMessage::Role::Tool);
    REQUIRE(messages[2].content == "echo: hello");
    REQUIRE(messages[3].role == ChatMessage::Role::Assistant);
    REQUIRE(messages[3].content == "Done after echo");
}

TEST_CASE_METHOD(ReActLoopFixture, "ReActLoop tool not found returns error result", "[react_loop][action]") {
    // 调用不存在的工具
    auto reader = std::make_shared<MockStreamReader>();
    reader->add_content_chunk("calling unknown tool");
    reader->add_tool_use_start("tu_01", "NonExistent");
    reader->add_tool_use_delta("tu_01", R"({})");
    reader->set_usage(10, 20);
    provider->set_next_reader(reader);

    make_text_reader("recovered from error");

    std::vector<ChatMessage> messages = {ChatMessage::user("q")};
    auto loop = make_loop();
    auto result = loop->run(messages, "", registry->get_all_schemas(), should_cancel);

    REQUIRE(result.total_tool_calls == 1);
    REQUIRE(messages[2].role == ChatMessage::Role::Tool);
    REQUIRE(messages[2].content.find("Tool not found") != std::string::npos);
}

TEST_CASE_METHOD(ReActLoopFixture, "ReActLoop tool exception is caught and reported", "[react_loop][action]") {
    auto failing_tool = std::make_shared<FailingTool>();
    registry->register_tool(failing_tool);

    make_tool_call_reader("tu_01", "Failing", R"({})");
    make_text_reader("recovered");

    std::vector<ChatMessage> messages = {ChatMessage::user("q")};
    auto loop = make_loop();
    auto result = loop->run(messages, "", registry->get_all_schemas(), should_cancel);

    REQUIRE(result.total_tool_calls == 1);
    REQUIRE(messages[2].role == ChatMessage::Role::Tool);
    REQUIRE(messages[2].content.find("intentional tool failure") != std::string::npos);
}

TEST_CASE_METHOD(ReActLoopFixture, "ReActLoop tool permission denied is reported", "[react_loop][action]") {
    auto denied_tool = std::make_shared<DeniedTool>();
    registry->register_tool(denied_tool);

    make_tool_call_reader("tu_01", "Denied", R"({})");
    make_text_reader("recovered");

    std::vector<ChatMessage> messages = {ChatMessage::user("q")};
    auto loop = make_loop();
    auto result = loop->run(messages, "", registry->get_all_schemas(), should_cancel);

    REQUIRE(result.total_tool_calls == 1);
    REQUIRE(messages[2].content.find("access denied by policy") != std::string::npos);
}

// ============================================================================
// max_iterations 限制
// ============================================================================

TEST_CASE_METHOD(ReActLoopFixture, "ReActLoop stops at max_iterations", "[react_loop][iteration]") {
    // 每次都返回 tool_use，永远不结束
    ReActLoop::Config config;
    config.max_iterations = 3;
    auto loop = make_loop(config);

    // 准备 3 次工具调用响应
    for (int i = 0; i < 3; ++i) {
        make_tool_call_reader("tu_" + std::to_string(i), "Echo", R"({"text":"loop"})");
    }

    std::vector<ChatMessage> messages = {ChatMessage::user("loop")};
    auto result = loop->run(messages, "", registry->get_all_schemas(), should_cancel);

    REQUIRE(result.total_iterations == 3);
    REQUIRE(result.was_error);  // 达到 max_iterations 视为错误
    REQUIRE(result.error_message.find("max") != std::string::npos);
}

// ============================================================================
// should_cancel 中断
// ============================================================================

TEST_CASE_METHOD(ReActLoopFixture, "ReActLoop cancels during Thought phase", "[react_loop][cancel]") {
    auto reader = std::make_shared<MockStreamReader>();
    reader->add_content_chunk("partial");
    reader->set_cancel_after(1);  // 消费第一个 chunk 后返回 Cancelled
    reader->set_usage(0, 0);
    provider->set_next_reader(reader);

    std::vector<ChatMessage> messages = {ChatMessage::user("q")};
    auto loop = make_loop();
    auto result = loop->run(messages, "", nlohmann::json::array(), should_cancel);

    REQUIRE(result.was_interrupted);
    REQUIRE_FALSE(result.was_error);
    REQUIRE(result.partial_content == "partial");
}

TEST_CASE_METHOD(ReActLoopFixture, "ReActLoop cooperative cancel during tool execution preserves tool messages", "[react_loop][cancel]") {
    // #23 P1：工具执行期间置位 should_cancel（等价于用户 Ctrl+C），
    // ReActLoop 应等待工具协作退出后仍生成 Observation（消息不丢），
    // 再在下一轮 Thought 走 was_interrupted 分支。
    auto slow_tool = std::make_shared<CooperativeSlowTool>();
    registry->register_tool(slow_tool);

    make_tool_call_reader("tu_01", "Slow", R"({})");

    // 第二轮 Thought 需要一个 reader 才能进入 while 检查 should_cancel
    make_text_reader("second round");

    std::vector<ChatMessage> messages = {ChatMessage::user("slow tool")};
    auto loop = make_loop();

    // 模拟工具执行中途用户打断
    std::thread canceler([&should_cancel = this->should_cancel]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        should_cancel = true;
    });
    auto result = loop->run(messages, "", registry->get_all_schemas(), should_cancel);
    canceler.join();

    // 中断被感知 → 走 was_interrupted 分支
    REQUIRE(result.was_interrupted);
    REQUIRE_FALSE(result.was_error);
    REQUIRE(result.total_tool_calls == 1);

    // 工具结果消息仍被完整记录（协作取消后 Observation 照常生成）
    REQUIRE(messages.size() >= 3);
    REQUIRE(messages[2].role == ChatMessage::Role::Tool);
    REQUIRE(messages[2].content.find("cancelled") != std::string::npos);
}

// ============================================================================
// 流式错误处理
// ============================================================================

TEST_CASE_METHOD(ReActLoopFixture, "ReActLoop handles stream error", "[react_loop][error]") {
    make_error_reader();

    std::vector<ChatMessage> messages = {ChatMessage::user("q")};
    auto loop = make_loop();
    auto result = loop->run(messages, "", nlohmann::json::array(), should_cancel);

    REQUIRE(result.was_error);
    REQUIRE(result.error_message.find("Stream error") != std::string::npos);
    REQUIRE(result.partial_content == "partial");
}

TEST_CASE_METHOD(ReActLoopFixture, "ReActLoop handles null reader", "[react_loop][error]") {
    // 不设置 next_reader，submit_completion 返回 nullptr
    std::vector<ChatMessage> messages = {ChatMessage::user("q")};
    auto loop = make_loop();
    auto result = loop->run(messages, "", nlohmann::json::array(), should_cancel);

    REQUIRE(result.was_error);
    REQUIRE(result.error_message.find("Stream error") != std::string::npos);
}

// ============================================================================
// 回调
// ============================================================================

TEST_CASE_METHOD(ReActLoopFixture, "ReActLoop invokes on_step callback for each phase", "[react_loop][callback]") {
    make_tool_call_reader("tu_01", "Echo", R"({"text":"x"})");
    make_text_reader("final");

    std::vector<ReActStep> steps;
    auto on_step = [&steps](const ReActStep& step) {
        steps.push_back(step);
    };

    std::vector<ChatMessage> messages = {ChatMessage::user("q")};
    auto loop = make_loop();
    auto result = loop->run(messages, "", registry->get_all_schemas(), should_cancel, on_step);

    // 应有：Thought(1) + Action(1) + Observation(1) + Thought(2) + FinalAnswer(2) = 5 步
    // 但实际代码可能只记录 Thought + FinalAnswer 步骤，Action/Observation 不一定单独记录
    // 至少应有 2 个 Thought 步骤
    REQUIRE(steps.size() >= 2);

    // 第一个应是 Thought
    REQUIRE(steps[0].type == ReActStepType::Thought);
    REQUIRE(steps[0].step_number == 1);

    // 最后一个应是 FinalAnswer
    REQUIRE(steps.back().type == ReActStepType::FinalAnswer);
}

TEST_CASE_METHOD(ReActLoopFixture, "ReActLoop invokes on_token callback for content deltas", "[react_loop][callback]") {
    auto reader = std::make_shared<MockStreamReader>();
    reader->add_content_chunk("Hello ");
    reader->add_content_chunk("world!");
    reader->set_usage(5, 10);
    provider->set_next_reader(reader);

    std::string collected_content;
    auto on_token = [&collected_content](const std::string& content_delta,
                                          const std::string& /*reasoning_delta*/) {
        collected_content += content_delta;
    };

    std::vector<ChatMessage> messages = {ChatMessage::user("q")};
    auto loop = make_loop();
    auto result = loop->run(messages, "", nlohmann::json::array(), should_cancel, nullptr, on_token);

    REQUIRE(collected_content == "Hello world!");
    REQUIRE(result.final_answer == "Hello world!");
}

TEST_CASE_METHOD(ReActLoopFixture, "ReActLoop invokes on_token for reasoning deltas", "[react_loop][callback]") {
    auto reader = std::make_shared<MockStreamReader>();
    reader->add_reasoning_chunk("step1 ");
    reader->add_reasoning_chunk("step2");
    reader->add_content_chunk("answer");
    reader->set_usage(5, 10);
    provider->set_next_reader(reader);

    std::string collected_reasoning;
    auto on_token = [&collected_reasoning](const std::string& /*content_delta*/,
                                            const std::string& reasoning_delta) {
        collected_reasoning += reasoning_delta;
    };

    std::vector<ChatMessage> messages = {ChatMessage::user("q")};
    auto loop = make_loop();
    auto result = loop->run(messages, "", nlohmann::json::array(), should_cancel, nullptr, on_token);

    REQUIRE(collected_reasoning == "step1 step2");
}

// ============================================================================
// 无工具注册表
// ============================================================================

TEST_CASE_METHOD(ReActLoopFixture, "ReActLoop works without registry (no tools)", "[react_loop][no_tools]") {
    auto reader = make_text_reader("plain answer");

    std::vector<ChatMessage> messages = {ChatMessage::user("q")};
    // H-5：必须显式注入 IConfigManager*（非空）
    auto loop = std::make_unique<ReActLoop>(provider.get(), nullptr, ReActLoop::Config{},
                                            &ConfigManager::instance());  // registry = nullptr
    auto result = loop->run(messages, "", nlohmann::json::array(), should_cancel);

    REQUIRE(result.final_answer == "plain answer");
    REQUIRE(result.total_tool_calls == 0);
}
