/**
 * @file test_agent_core.cpp
 * @brief 0.6.x Agent 架构核心测试（#31 目标导向 + #33 类型体系）
 * @details 验证：
 *          - parse_agent_type：agent.active 别名解析（含 skill 过滤兼容）
 *          - parse_goal：agent.goal 目标串解析
 *          - Verdict：check_goal 各类目标验证（FileExists / CustomScript 实证；
 *            TestsPass/BuildClean 因依赖真实构建环境，用可注入命令验证）
 * @version 1.0.0
 * @date 2026-08
 */

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include "agent/core/agent_type.h"
#include "agent/core/goal_verdict.h"
#include "agent/core/verdict.h"

namespace fs = std::filesystem;
using namespace agent;

// ============================================================
// parse_agent_type — 类型解析与别名
// ============================================================

TEST_CASE("parse_agent_type: 空串与默认回退 ReAct", "[agent][agent_type]") {
    REQUIRE(parse_agent_type("") == AgentType::ReAct);
    REQUIRE(parse_agent_type("  ") == AgentType::ReAct);
    REQUIRE(parse_agent_type("react") == AgentType::ReAct);
    REQUIRE(parse_agent_type("ReAct") == AgentType::ReAct);
    REQUIRE(parse_agent_type("explore") == AgentType::ReAct);  // #33 角色别名
}

TEST_CASE("parse_agent_type: goal-guarded 别名", "[agent][agent_type]") {
    REQUIRE(parse_agent_type("goal-guarded") == AgentType::GoalGuarded);
    REQUIRE(parse_agent_type("goalguarded") == AgentType::GoalGuarded);
    REQUIRE(parse_agent_type("verify") == AgentType::GoalGuarded);
    REQUIRE(parse_agent_type("  Verify  ") == AgentType::GoalGuarded);  // 大小写+空白
}

TEST_CASE("parse_agent_type: 占位类型路由可识别", "[agent][agent_type]") {
    REQUIRE(parse_agent_type("planner") == AgentType::Planner);
    REQUIRE(parse_agent_type("coordinator") == AgentType::Coordinator);
    REQUIRE(parse_agent_type("researcher") == AgentType::Researcher);
    REQUIRE(parse_agent_type("reviewer") == AgentType::Reviewer);
    REQUIRE(parse_agent_type("batch") == AgentType::Batch);
    REQUIRE(parse_agent_type("watch") == AgentType::Watch);
}

TEST_CASE("parse_agent_type: 未知串 → Unknown（不抛异常）", "[agent][agent_type]") {
    REQUIRE(parse_agent_type("bogus-mode") == AgentType::Unknown);
    REQUIRE(parse_agent_type("666") == AgentType::Unknown);
}

TEST_CASE("to_string / is_implemented 一致", "[agent][agent_type]") {
    REQUIRE(to_string(AgentType::GoalGuarded) == "goal-guarded");
    REQUIRE(is_implemented(AgentType::ReAct));
    REQUIRE(is_implemented(AgentType::GoalGuarded));
    REQUIRE_FALSE(is_implemented(AgentType::Coordinator));  // 占位未实现
    REQUIRE_FALSE(is_implemented(AgentType::Unknown));
}

// ============================================================
// parse_goal — 目标解析
// ============================================================

TEST_CASE("parse_goal: 各类型解析", "[agent][goal][verify]") {
    REQUIRE(parse_goal("").type == AgentGoal::None);
    REQUIRE(parse_goal("  ").type == AgentGoal::None);
    REQUIRE(parse_goal("tests_pass").type == AgentGoal::TestsPass);
    REQUIRE(parse_goal("Tests").type == AgentGoal::TestsPass);
    REQUIRE(parse_goal("build_clean").type == AgentGoal::BuildClean);
    REQUIRE(parse_goal("lint_zero").type == AgentGoal::LintZero);
    REQUIRE(parse_goal("bogus").type == AgentGoal::None);  // 未知回退 None
}

TEST_CASE("parse_goal: file_exists 提取路径", "[agent][goal]") {
    AgentGoal g = parse_goal("file_exists:algo.txt");
    REQUIRE(g.type == AgentGoal::FileExists);
    REQUIRE(g.path == "algo.txt");
}

TEST_CASE("parse_goal: cmd 提取命令", "[agent][goal]") {
    AgentGoal g = parse_goal("cmd:ctest --output-on-failure");
    REQUIRE(g.type == AgentGoal::CustomScript);
    REQUIRE(g.command == "ctest --output-on-failure");
}

// ============================================================
// Verdict — check_goal 目标验证
// ============================================================

namespace {

inline std::string test_dir() {
    return (fs::temp_directory_path() / ("workx_verdict_test_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
        .string();
}

} // namespace

TEST_CASE("verdict: FileExists 达成/未达成", "[agent][verify]") {
    const std::string dir = test_dir();
    fs::create_directories(dir);
    const std::string target = dir + "/target.txt";
    // 未达成
    AgentGoal missing;
    missing.type = AgentGoal::FileExists;
    missing.path = target;
    Verdict v1 = check_goal(missing, dir);
    REQUIRE(v1.status == GoalStatus::Pending);

    // 达成
    std::ofstream(target) << "x";
    REQUIRE(fs::exists(target));
    Verdict v2 = check_goal(missing, dir);
    REQUIRE(v2.status == GoalStatus::Achieved);

    fs::remove_all(dir);
}

TEST_CASE("verdict: CustomScript 退出码判定", "[agent][verify]") {
    const std::string dir = test_dir();
    fs::create_directories(dir);

    // 成功脚本（退出 0）→ Achieved
    AgentGoal ok_goal;
    ok_goal.type = AgentGoal::CustomScript;
#ifdef _WIN32
    ok_goal.command = "cmd /C exit 0";
#else
    ok_goal.command = "true";
#endif
    Verdict v_ok = check_goal(ok_goal, dir);
    REQUIRE(v_ok.status == GoalStatus::Achieved);

    // 失败脚本（退出 1）→ Pending
    AgentGoal bad_goal;
    bad_goal.type = AgentGoal::CustomScript;
#ifdef _WIN32
    bad_goal.command = "cmd /C exit 1";
#else
    bad_goal.command = "false";
#endif
    Verdict v_bad = check_goal(bad_goal, dir);
    REQUIRE(v_bad.status == GoalStatus::Pending);

    fs::remove_all(dir);
}

TEST_CASE("verdict: None 目标 → Pending", "[agent][verify]") {
    AgentGoal none_goal;  // type = None
    Verdict v = check_goal(none_goal, ".");
    REQUIRE(v.status == GoalStatus::Pending);
}

TEST_CASE("verdict: 缺少验证器/错误配置 → Failed", "[agent][verify]") {
    // file_exists 缺 path → Failed
    AgentGoal no_path;
    no_path.type = AgentGoal::FileExists;
    REQUIRE(check_goal(no_path, ".").status == GoalStatus::Failed);
    // custom_script 缺 command → Failed
    AgentGoal no_cmd;
    no_cmd.type = AgentGoal::CustomScript;
    REQUIRE(check_goal(no_cmd, ".").status == GoalStatus::Failed);
}