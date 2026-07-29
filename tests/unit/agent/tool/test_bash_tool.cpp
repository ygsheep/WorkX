/**
 * @file test_bash_tool.cpp
 * @brief BashTool 单元测试
 * @details 覆盖：
 *          - 元信息（name/description/prompt/schema）
 *          - 参数校验（缺 command / 空 command）
 *          - 同步执行（echo 命令）
 *          - 超时/取消路径
 *          - 后台任务（TaskManager nullptr 检查）
 *          - 进度回调（report_progress 触发）
 *
 *          跨平台命令选择：
 *          - Windows: cmd.exe /c echo hello
 *          - POSIX:   /bin/sh -c "echo hello"
 *
 *          所有测试都用 PATH 中必定存在的命令，避免依赖项目布局。
 */

#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "agent/tool/BashTool/bash_tool.h"
#include "agent/tool/context.h"
#include "agent/tool/result.h"
#include "core/config/config_manager.h"
#include "core/utils/error.h"

using namespace agent;
using namespace agent::tool;
using namespace std::chrono_literals;

namespace {

/// 构建最小可用 ToolContext（注入 ConfigManager 单例，无 TaskManager）
/// 注意：ToolContext 含 std::atomic 不可拷贝，需通过传入引用方式构造
void fill_ctx(ToolContext& ctx) {
    ctx.cwd = std::filesystem::current_path().string();
    ctx.session_id = "test";
    ctx.config_manager_ptr = &ConfigManager::instance();
    // task_manager_ptr 故意留 nullptr（后台任务测试用）
}

} // namespace

// ============================================================
// 元信息
// ============================================================

TEST_CASE("BashTool name is Bash", "[tool][bash][metadata]") {
    BashTool tool;
    REQUIRE(tool.name() == "Bash");
}

TEST_CASE("BashTool description non-empty", "[tool][bash][metadata]") {
    BashTool tool;
    REQUIRE_FALSE(tool.description().empty());
}

TEST_CASE("BashTool prompt non-empty", "[tool][bash][metadata]") {
    BashTool tool;
    REQUIRE_FALSE(tool.prompt().empty());
}

TEST_CASE("BashTool input_schema has command required", "[tool][bash][metadata]") {
    BashTool tool;
    auto schema = tool.input_schema();
    REQUIRE(schema["type"] == "object");
    REQUIRE(schema["properties"].contains("command"));
    // required 是数组，检查元素是否存在
    bool command_required = false;
    for (const auto& item : schema["required"]) {
        if (item.get<std::string>() == "command") {
            command_required = true;
            break;
        }
    }
    REQUIRE(command_required);
    // 验证新增字段
    REQUIRE(schema["properties"].contains("run_in_background"));
    REQUIRE(schema["properties"].contains("dangerously_disable_sandbox"));
    REQUIRE(schema["properties"].contains("timeout"));
    REQUIRE(schema["properties"].contains("description"));
}

// ============================================================
// 参数校验
// ============================================================

TEST_CASE("BashTool rejects missing command", "[tool][bash][validation]") {
    BashTool tool;
    ToolContext ctx; fill_ctx(ctx);
    nlohmann::json input = {{"timeout", 1000}};

    auto r = tool.call(input, ctx);
    REQUIRE(r.is_err());
    REQUIRE(r.error().code == Error::Code::MissingArgument);
}

TEST_CASE("BashTool rejects empty command", "[tool][bash][validation]") {
    BashTool tool;
    ToolContext ctx; fill_ctx(ctx);
    nlohmann::json input = {{"command", ""}};

    auto r = tool.call(input, ctx);
    REQUIRE(r.is_err());
    REQUIRE(r.error().code == Error::Code::InvalidInput);
}

TEST_CASE("BashTool rejects non-string command", "[tool][bash][validation]") {
    BashTool tool;
    ToolContext ctx; fill_ctx(ctx);
    nlohmann::json input = {{"command", 123}};

    auto r = tool.call(input, ctx);
    REQUIRE(r.is_err());
    REQUIRE(r.error().code == Error::Code::MissingArgument);
}

// ============================================================
// 同步执行
// ============================================================

TEST_CASE("BashTool executes echo command successfully", "[tool][bash][sync]") {
    BashTool tool;
    ToolContext ctx; fill_ctx(ctx);
    nlohmann::json input = {{"command", "echo hello_world"}};

    auto r = tool.call(input, ctx);
    REQUIRE(r.is_ok());
    // 输出应包含 hello_world
    REQUIRE(r.value().text.find("hello_world") != std::string::npos);
    // 应包含 <stdout> 标签
    REQUIRE(r.value().text.find("<stdout>") != std::string::npos);
}

TEST_CASE("BashTool reports non-zero exit code", "[tool][bash][sync]") {
    BashTool tool;
    ToolContext ctx; fill_ctx(ctx);
#ifdef _WIN32
    nlohmann::json input = {{"command", "exit /b 42"}};
#else
    nlohmann::json input = {{"command", "exit 42"}};
#endif

    auto r = tool.call(input, ctx);
    REQUIRE(r.is_ok());
    REQUIRE(r.value().text.find("42") != std::string::npos);
    REQUIRE(r.value().text.find("<error>") != std::string::npos);
}

TEST_CASE("BashTool handles timeout", "[tool][bash][sync]") {
    BashTool tool;
    ToolContext ctx; fill_ctx(ctx);
#ifdef _WIN32
    nlohmann::json input = {{"command", "ping -n 10 127.0.0.1"}, {"timeout", 200}};
#else
    nlohmann::json input = {{"command", "sleep 10"}, {"timeout", 200}};
#endif

    auto r = tool.call(input, ctx);
    REQUIRE(r.is_ok());
    REQUIRE(r.value().text.find("timed out") != std::string::npos);
}

TEST_CASE("BashTool respects cancellation", "[tool][bash][sync]") {
    BashTool tool;
    ToolContext ctx; fill_ctx(ctx);
    std::atomic<bool> cancel{true};
    ctx.cancel_flag = &cancel;

    nlohmann::json input = {{"command", "echo should_not_run"}};
    auto r = tool.call(input, ctx);
    REQUIRE(r.is_err());
    REQUIRE(r.error().code == Error::Code::Cancelled);
}

// ============================================================
// 后台任务
// ============================================================

TEST_CASE("BashTool background requires TaskManager", "[tool][bash][background]") {
    BashTool tool;
    ToolContext ctx; fill_ctx(ctx);  // task_manager_ptr = nullptr

    nlohmann::json input = {
        {"command", "echo bg"},
        {"run_in_background", true}
    };

    auto r = tool.call(input, ctx);
    REQUIRE(r.is_err());
    REQUIRE(r.error().code == Error::Code::ConfigInvalid);
}

// ============================================================
// 进度回调
// ============================================================

TEST_CASE("BashTool reports progress via callback", "[tool][bash][progress]") {
    BashTool tool;
    ToolContext ctx; fill_ctx(ctx);

    std::vector<std::string> progress_reports;
    ctx.progress_callback = [&progress_reports](const std::string& text) {
        progress_reports.push_back(text);
    };

    nlohmann::json input = {{"command", "echo progress_test"}};
    auto r = tool.call(input, ctx);
    REQUIRE(r.is_ok());
    // 应至少上报 2 次（开始 + 完成）
    REQUIRE(progress_reports.size() >= 2);
    // 第一次包含 "Executing"
    REQUIRE(progress_reports[0].find("Executing") != std::string::npos);
}

// ============================================================
// 沙盒配置
// ============================================================

TEST_CASE("BashTool accepts dangerously_disable_sandbox", "[tool][bash][sandbox]") {
    BashTool tool;
    ToolContext ctx; fill_ctx(ctx);

    // 禁用沙盒不应导致错误（实际是否降级由平台决定）
    nlohmann::json input = {
        {"command", "echo no_sandbox"},
        {"dangerously_disable_sandbox", true}
    };

    auto r = tool.call(input, ctx);
    REQUIRE(r.is_ok());
    REQUIRE(r.value().text.find("no_sandbox") != std::string::npos);
}

// ============================================================
// 超时上限
// ============================================================

TEST_CASE("BashTool clamps timeout to max", "[tool][bash][timeout]") {
    BashTool tool;
    ToolContext ctx; fill_ctx(ctx);

    // 设置超过上限的 timeout，应被截断为 600000ms
    nlohmann::json input = {
        {"command", "echo max_timeout"},
        {"timeout", 999999999}
    };

    auto r = tool.call(input, ctx);
    REQUIRE(r.is_ok());
    REQUIRE(r.value().text.find("max_timeout") != std::string::npos);
}
