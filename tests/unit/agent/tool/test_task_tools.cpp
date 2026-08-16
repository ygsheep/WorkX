/**
 * @file test_task_tools.cpp
 * @brief #26：AgentTool / TaskStopTool / TaskOutputTool 与 Task 输出缓冲
 * @date 2026-08
 */

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <algorithm>
#include <vector>

#include "agent/tool/AgentTool/agent_tool.h"
#include "agent/tool/Task/task_output_tool.h"
#include "agent/tool/Task/task_stop_tool.h"
#include "agent/tool/registry.h"
#include "agent/tool/itool.h"
#include "core/task/task_manager.h"
#include "core/task/task_events.h"
#include "core/events/agent_events.h"

#include "helpers/mock_config_manager.h"
#include "helpers/mock_event_bus.h"
#include "helpers/mock_provider.h"

using namespace agent;
using namespace agent::tool;
using namespace agent::test;

namespace {

/// @brief 任务工具测试夹具：清理单例残留
struct TaskToolsFixture {
    TaskToolsFixture() {
        EventBus::instance().clear();
        TaskManager::instance().cancelAll();
        TaskManager::instance().waitForAll();
        TaskManager::instance().update();
    }
    ~TaskToolsFixture() {
        TaskManager::instance().cancelAll();
        TaskManager::instance().waitForAll();
        TaskManager::instance().update();
        EventBus::instance().clear();
    }
};

/// @brief 填充最小 ToolContext（任务工具需要 task_manager/config_manager）
void fill_ctx(ToolContext& ctx, MockEventBus& bus, ITaskManager& tm, IConfigManager& cfg,
              ICompletionProvider* provider = nullptr) {
    ctx.cwd = ".";
    ctx.session_id = "test-session";
    ctx.task_manager_ptr = &tm;
    ctx.config_manager_ptr = &cfg;
    ctx.event_bus_ptr = &bus;
    ctx.provider_ptr = provider;
}

/// @brief Stub 只读工具（模拟 Read，is_read_only() = true）
class StubReadOnlyTool : public ITool {
public:
    const std::string& name() const override { static const std::string n{"Read"}; return n; }
    const std::string& description() const override { static const std::string d{"read"}; return d; }
    const std::string& prompt() const override { static const std::string p; return p; }
    nlohmann::json input_schema() const override { return {{"type", "object"}}; }
    bool is_read_only() const override { return true; }
    ResultV2<ToolResult> call(const nlohmann::json&, const ToolContext&) const override {
        return ResultV2<ToolResult>::ok(ToolResult::ok(std::string("read-ok")));
    }
};

/// @brief Stub 可写工具（模拟 Bash，is_read_only() = false）
class StubWriteTool : public ITool {
public:
    const std::string& name() const override { static const std::string n{"Bash"}; return n; }
    const std::string& description() const override { static const std::string d{"write"}; return d; }
    const std::string& prompt() const override { static const std::string p; return p; }
    nlohmann::json input_schema() const override { return {{"type", "object"}}; }
    ResultV2<ToolResult> call(const nlohmann::json&, const ToolContext&) const override {
        return ResultV2<ToolResult>::ok(ToolResult::ok(std::string("write-ok")));
    }
};

/// @brief Stub Agent 工具（模拟 AgentTool 本体，name = "Agent"），供防递归测试
class StubAgentTool : public ITool {
public:
    const std::string& name() const override { static const std::string n{"Agent"}; return n; }
    const std::string& description() const override { static const std::string d{"agent"}; return d; }
    const std::string& prompt() const override { static const std::string p; return p; }
    nlohmann::json input_schema() const override { return {{"type", "object"}}; }
    ResultV2<ToolResult> call(const nlohmann::json&, const ToolContext&) const override {
        return ResultV2<ToolResult>::ok(ToolResult::ok(std::string("agent-ok")));
    }
};

} // namespace

// ============================================================
// Task 输出缓冲 + TaskOutputEvent
// ============================================================

TEST_CASE("Task append_output accumulates output and publishes TaskOutputEvent", "[task][output]") {
    MockEventBus bus;
    bus.set_dispatch_enabled(true);

    std::vector<std::string> lines;
    bus.subscribe<TaskOutputEvent>([&lines](const TaskOutputEvent& e) {
        lines.push_back(e.line);
    });

    auto task = std::make_shared<Task>("t1", [](const std::atomic<bool>&) {}, bus);
    task->append_output("hello");
    task->append_output("world");

    REQUIRE(task->output() == "hello\nworld\n");
    // 异步事件需 drain 后派发
    bus.drain_async_events();
    REQUIRE(lines.size() == 2);
    REQUIRE(lines[0] == "hello");
    REQUIRE(lines[1] == "world");
}

// ============================================================
// AgentTool
// ============================================================

TEST_CASE_METHOD(TaskToolsFixture, "AgentTool rejects missing prompt", "[agent_tool]") {
    MockEventBus bus;
    auto& tm = TaskManager::instance();
    MockConfigManager cfg;
    ToolContext ctx;
    fill_ctx(ctx, bus, tm, cfg);
    AgentTool tool;

    auto r = tool.call(nlohmann::json{{"prompt", ""}}, ctx);
    REQUIRE(r.is_err());
    REQUIRE(r.error().code == Error::Code::MissingArgument);
}

TEST_CASE_METHOD(TaskToolsFixture, "AgentTool requires provider", "[agent_tool]") {
    MockEventBus bus;
    auto& tm = TaskManager::instance();
    MockConfigManager cfg;
    ToolContext ctx;
    fill_ctx(ctx, bus, tm, cfg);
    AgentTool tool;

    auto r = tool.call(nlohmann::json{{"prompt", "do something"}}, ctx);
    REQUIRE(r.is_err());
}

TEST_CASE_METHOD(TaskToolsFixture, "AgentTool launches sub-agent in background and writes output", "[agent_tool]") {
    MockEventBus bus;
    auto& tm = TaskManager::instance();
    MockConfigManager cfg;
    ToolContext ctx;

    auto provider = std::make_shared<MockCompletionProvider>();
    auto reader = std::make_shared<MockStreamReader>();
    reader->add_content_chunk("done result");
    provider->set_next_reader(reader);
    fill_ctx(ctx, bus, tm, cfg, provider.get());

    AgentTool tool;
    auto r = tool.call(nlohmann::json{{"prompt", "summarize the file"}}, ctx);
    REQUIRE(r.is_ok());
    REQUIRE(r.value().text.find("Sub-agent launched") != std::string::npos);

    // 后台任务已注册并执行
    auto tasks = tm.getTasks();
    REQUIRE(tasks.size() == 1);
    auto& task = tasks[0];
    REQUIRE(task->getName().size() == 9);  // 'a' + 8 随机字符
    REQUIRE(task->getName()[0] == 'a');

    tm.wait(task);
    REQUIRE(task->isFinished());
    // 输出缓冲含最终答案（step 行 + 收尾行）
    REQUIRE(task->output().find("done result") != std::string::npos);
}

TEST_CASE_METHOD(TaskToolsFixture, "AgentTool background completion publishes SubAgentCompletedEvent", "[agent_tool][review]") {
    MockEventBus bus;
    bus.set_dispatch_enabled(true);  // 同步派发，便于断言

    std::vector<SubAgentCompletedEvent> completions;
    bus.subscribe<SubAgentCompletedEvent>([&completions](const SubAgentCompletedEvent& e) {
        completions.push_back(e);
    });

    auto& tm = TaskManager::instance();
    MockConfigManager cfg;
    ToolContext ctx;

    auto provider = std::make_shared<MockCompletionProvider>();
    auto reader = std::make_shared<MockStreamReader>();
    reader->add_content_chunk("auto feedback result");
    provider->set_next_reader(reader);
    fill_ctx(ctx, bus, tm, cfg, provider.get());

    AgentTool tool;
    auto r = tool.call(nlohmann::json{{"prompt", "background task"}}, ctx);
    REQUIRE(r.is_ok());

    // 等待后台任务完成（完成事件在子 Agent 收尾时发布）
    auto tasks = tm.getTasks();
    REQUIRE(tasks.size() == 1);
    tm.wait(tasks[0]);
    bus.drain_async_events();

    // 自动回送事件已发布：携带 task_id 与最终结果摘要
    REQUIRE(completions.size() == 1);
    REQUIRE(completions[0].task_id == tasks[0]->getName());
    REQUIRE(completions[0].final_answer.find("Final: auto feedback result") != std::string::npos);
    REQUIRE_FALSE(completions[0].was_error);
}

TEST_CASE_METHOD(TaskToolsFixture, "AgentTool synchronous mode returns completed result", "[agent_tool]") {
    MockEventBus bus;
    auto& tm = TaskManager::instance();
    MockConfigManager cfg;
    ToolContext ctx;

    auto provider = std::make_shared<MockCompletionProvider>();
    auto reader = std::make_shared<MockStreamReader>();
    reader->add_content_chunk("sync answer");
    provider->set_next_reader(reader);
    fill_ctx(ctx, bus, tm, cfg, provider.get());

    AgentTool tool;
    auto r = tool.call(nlohmann::json{{"prompt", "quick task"}, {"run_in_background", false}}, ctx);
    REQUIRE(r.is_ok());
    REQUIRE(r.value().text.find("Sub-agent completed") != std::string::npos);
    REQUIRE(r.value().text.find("sync answer") != std::string::npos);
}

TEST_CASE_METHOD(TaskToolsFixture, "AgentTool inherits parent permission mode (Plan read-only)", "[agent_tool][review]") {
    MockEventBus bus;
    auto& tm = TaskManager::instance();
    MockConfigManager cfg;
    ToolContext ctx;

    auto provider = std::make_shared<MockCompletionProvider>();
    auto reader = std::make_shared<MockStreamReader>();
    reader->add_content_chunk("plan result");
    provider->set_next_reader(reader);
    fill_ctx(ctx, bus, tm, cfg, provider.get());
    ctx.permission_mode = PermissionMode::Plan;  // 父会话处于 Plan（只读）

    AgentTool tool;
    // 评审 #1：父 Plan 模式下仍允许启动，但子 Agent 继承 Plan 只读权限，
    // 不会以 Default 全权执行（写/执行被拒绝）
    auto r = tool.call(nlohmann::json{{"prompt", "research only"}}, ctx);
    REQUIRE(r.is_ok());
    REQUIRE(r.value().text.find("Sub-agent") != std::string::npos);

    auto tasks = tm.getTasks();
    REQUIRE(tasks.size() == 1);
    tm.wait(tasks[0]);
    REQUIRE(tasks[0]->isFinished());
}

TEST_CASE_METHOD(TaskToolsFixture, "AgentTool Plan mode gives sub-agent read-only tool set", "[agent_tool][review]") {
    MockEventBus bus;
    auto& tm = TaskManager::instance();
    MockConfigManager cfg;
    ToolContext ctx;

    auto provider = std::make_shared<MockCompletionProvider>();
    auto reader = std::make_shared<MockStreamReader>();
    reader->add_content_chunk("readonly result");
    provider->set_next_reader(reader);
    fill_ctx(ctx, bus, tm, cfg, provider.get());
    ctx.permission_mode = PermissionMode::Plan;  // 父会话处于 Plan（只读）

    // 父 registry 同时含只读(Read)与可写(Bash)工具
    auto registry = std::make_shared<ToolRegistry>();
    registry->register_tool(std::make_shared<StubReadOnlyTool>());
    registry->register_tool(std::make_shared<StubWriteTool>());
    ctx.tool_registry = registry;

    AgentTool tool;
    auto r = tool.call(nlohmann::json{{"prompt", "research only"}}, ctx);
    REQUIRE(r.is_ok());

    auto tasks = tm.getTasks();
    REQUIRE(tasks.size() == 1);
    tm.wait(tasks[0]);
    REQUIRE(tasks[0]->isFinished());

    // 子 Agent 收到的工具集仅含只读工具 Read，写工具 Bash 被过滤
    REQUIRE(provider->last_tools.is_array());
    REQUIRE(provider->last_tools.size() == 1);
    REQUIRE(provider->last_tools[0]["name"] == "Read");
}

TEST_CASE_METHOD(TaskToolsFixture, "AgentTool applies tools whitelist", "[agent_tool]") {
    MockEventBus bus;
    auto& tm = TaskManager::instance();
    MockConfigManager cfg;
    ToolContext ctx;

    auto provider = std::make_shared<MockCompletionProvider>();
    auto reader = std::make_shared<MockStreamReader>();
    reader->add_content_chunk("whitelisted result");
    provider->set_next_reader(reader);
    fill_ctx(ctx, bus, tm, cfg, provider.get());
    // 默认权限模式（Default，非只读）

    auto registry = std::make_shared<ToolRegistry>();
    registry->register_tool(std::make_shared<StubReadOnlyTool>());
    registry->register_tool(std::make_shared<StubWriteTool>());
    ctx.tool_registry = registry;

    AgentTool tool;
    auto r = tool.call(nlohmann::json{
        {"prompt", "task"}, {"tools", nlohmann::json::array({"Read", "Bash"})}}, ctx);
    REQUIRE(r.is_ok());

    auto tasks = tm.getTasks();
    REQUIRE(tasks.size() == 1);
    tm.wait(tasks[0]);
    REQUIRE(tasks[0]->isFinished());

    // 白名单生效：仅含 Read 与 Bash（发送前按 name 排序，故不依赖顺序）
    REQUIRE(provider->last_tools.is_array());
    REQUIRE(provider->last_tools.size() == 2);
    std::vector<std::string> got;
    for (const auto& s : provider->last_tools) {
        got.push_back(s["name"].get<std::string>());
    }
    REQUIRE(std::find(got.begin(), got.end(), "Read") != got.end());
    REQUIRE(std::find(got.begin(), got.end(), "Bash") != got.end());
}

TEST_CASE_METHOD(TaskToolsFixture, "AgentTool excludes Agent tool to prevent recursion", "[agent_tool][review]") {
    MockEventBus bus;
    auto& tm = TaskManager::instance();
    MockConfigManager cfg;
    ToolContext ctx;

    auto provider = std::make_shared<MockCompletionProvider>();
    auto reader = std::make_shared<MockStreamReader>();
    reader->add_content_chunk("no recursion");
    provider->set_next_reader(reader);
    fill_ctx(ctx, bus, tm, cfg, provider.get());

    // 父 registry 同时含 Agent 工具与其他工具；即使白名单显式包含 "Agent" 也应被忽略
    auto registry = std::make_shared<ToolRegistry>();
    registry->register_tool(std::make_shared<StubAgentTool>());
    registry->register_tool(std::make_shared<StubReadOnlyTool>());
    registry->register_tool(std::make_shared<StubWriteTool>());
    ctx.tool_registry = registry;

    AgentTool tool;
    auto r = tool.call(nlohmann::json{
        {"prompt", "task"}, {"tools", nlohmann::json::array({"Agent", "Read", "Bash"})}}, ctx);
    REQUIRE(r.is_ok());

    auto tasks = tm.getTasks();
    REQUIRE(tasks.size() == 1);
    tm.wait(tasks[0]);
    REQUIRE(tasks[0]->isFinished());

    // 子 Agent 工具集不含 Agent 工具（防无限递归），仅保留 Read/Bash
    REQUIRE(provider->last_tools.is_array());
    REQUIRE(provider->last_tools.size() == 2);
    for (const auto& s : provider->last_tools) {
        REQUIRE(s["name"].get<std::string>() != "Agent");
    }
}

TEST_CASE_METHOD(TaskToolsFixture, "TaskStopTool stops a running task", "[task_stop][slow]") {
    MockEventBus bus;
    auto& tm = TaskManager::instance();
    MockConfigManager cfg;
    ToolContext ctx;
    fill_ctx(ctx, bus, tm, cfg);
    TaskStopTool tool;

    auto task = tm.launch("agent-run-1",
        [](const std::atomic<bool>&) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
        });

    // 等待任务进入运行态
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto r = tool.call(nlohmann::json{{"task_id", "agent-run-1"}}, ctx);
    REQUIRE(r.is_ok());
    REQUIRE(r.value().text.find("Successfully stopped task: agent-run-1")
            != std::string::npos);

    // 任务最终进入 Cancelled
    tm.wait(task);
    REQUIRE(task->getStatus() == TaskStatus::Cancelled);
}

TEST_CASE_METHOD(TaskToolsFixture, "TaskStopTool rejects unknown and finished tasks", "[task_stop]") {
    MockEventBus bus;
    auto& tm = TaskManager::instance();
    MockConfigManager cfg;
    ToolContext ctx;
    fill_ctx(ctx, bus, tm, cfg);
    TaskStopTool tool;

    // 未知任务
    auto r1 = tool.call(nlohmann::json{{"task_id", "nope"}}, ctx);
    REQUIRE(r1.is_err());
    REQUIRE(r1.error().code == Error::Code::ResourceNotFound);

    // 已结束任务（立即完成的 no-op 任务）
    auto task = tm.launch("done-task", [](const std::atomic<bool>&) {});
    tm.wait(task);
    auto r2 = tool.call(nlohmann::json{{"task_id", "done-task"}}, ctx);
    REQUIRE(r2.is_err());
}

TEST_CASE_METHOD(TaskToolsFixture, "TaskStopTool rejects missing task_id", "[task_stop]") {
    MockEventBus bus;
    auto& tm = TaskManager::instance();
    MockConfigManager cfg;
    ToolContext ctx;
    fill_ctx(ctx, bus, tm, cfg);
    TaskStopTool tool;

    // 缺 task_id（空对象）
    auto r = tool.call(nlohmann::json::object(), ctx);
    REQUIRE(r.is_err());
    REQUIRE(r.error().code == Error::Code::MissingArgument);
}

// ============================================================
// TaskOutputTool
// ============================================================

TEST_CASE_METHOD(TaskToolsFixture, "TaskOutputTool reads output of finished task", "[task_output]") {
    MockEventBus bus;
    auto& tm = TaskManager::instance();
    MockConfigManager cfg;
    ToolContext ctx;
    fill_ctx(ctx, bus, tm, cfg);
    TaskOutputTool tool;

    auto task = tm.launch("agent-out-1", [](const std::atomic<bool>&) {});
    task->append_output("step one");
    task->append_output("Final: answer");
    tm.wait(task);

    auto r = tool.call(nlohmann::json{{"task_id", "agent-out-1"}, {"block", true}, {"timeout", 2000}}, ctx);
    REQUIRE(r.is_ok());
    REQUIRE(r.value().text.find("\"status\":\"completed\"") != std::string::npos);
    REQUIRE(r.value().text.find("step one") != std::string::npos);
    REQUIRE(r.value().text.find("Final: answer") != std::string::npos);
}

TEST_CASE_METHOD(TaskToolsFixture, "TaskOutputTool waits for running task", "[task_output]") {
    MockEventBus bus;
    auto& tm = TaskManager::instance();
    MockConfigManager cfg;
    ToolContext ctx;
    fill_ctx(ctx, bus, tm, cfg);
    TaskOutputTool tool;

    auto task = tm.launch("agent-slow",
        [](const std::atomic<bool>&) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        });

    auto r = tool.call(nlohmann::json{{"task_id", "agent-slow"}, {"block", true}, {"timeout", 3000}}, ctx);
    REQUIRE(r.is_ok());
    REQUIRE(r.value().text.find("\"status\":\"completed\"") != std::string::npos);
}

TEST_CASE_METHOD(TaskToolsFixture, "TaskOutputTool times out on long task", "[task_output][slow]") {
    MockEventBus bus;
    auto& tm = TaskManager::instance();
    MockConfigManager cfg;
    ToolContext ctx;
    fill_ctx(ctx, bus, tm, cfg);
    TaskOutputTool tool;

    auto task = tm.launch("agent-long",
        [](const std::atomic<bool>&) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
        });

    auto r = tool.call(nlohmann::json{{"task_id", "agent-long"}, {"block", true}, {"timeout", 200}}, ctx);
    REQUIRE(r.is_ok());
    REQUIRE(r.value().text.find("\"timed_out\":true") != std::string::npos);

    // 清理：取消长任务避免拖慢测试
    tm.cancel(task);
}

TEST_CASE_METHOD(TaskToolsFixture, "TaskOutputTool rejects unknown and missing task_id", "[task_output]") {
    MockEventBus bus;
    auto& tm = TaskManager::instance();
    MockConfigManager cfg;
    ToolContext ctx;
    fill_ctx(ctx, bus, tm, cfg);
    TaskOutputTool tool;

    auto r1 = tool.call(nlohmann::json{{"task_id", "nope"}}, ctx);
    REQUIRE(r1.is_err());
    REQUIRE(r1.error().code == Error::Code::ResourceNotFound);

    auto r2 = tool.call(nlohmann::json::object(), ctx);
    REQUIRE(r2.is_err());
    REQUIRE(r2.error().code == Error::Code::MissingArgument);
}
