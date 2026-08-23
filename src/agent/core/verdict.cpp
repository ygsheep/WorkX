#include "agent/core/verdict.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <string_view>

#include "core/process/exec_output.h"
#include "core/process/subprocess.h"
#include "liblogger/logger.h"

namespace agent {

namespace {

/// @brief 去首尾空白并转小写（供目标串前缀匹配）
std::string trimmed_lower(std::string_view s) {
    std::string out(s);
    const auto not_space = [](unsigned char c) { return !std::isspace(c); };
    auto first = std::find_if(out.begin(), out.end(), not_space);
    auto last = std::find_if(out.rbegin(), out.rend(), not_space).base();
    if (first >= last) {
        out.clear();  // 全空白：避免 std::string(first, last) 用 first>last 构造（UB）
        return out;
    }
    out = std::string(first, last);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

} // namespace

/// @brief 执行命令并返回退出码；启动失败返回 -1
int run_exit_code(const std::string& cmd, const std::string& cwd) {
    using namespace agent::process;
    // exec() 的 cmd 必须是纯可执行名、参数走 args；命令字符串需经 shell 包装
    //（cmd：cmd.exe /d /s /c；POSIX：sh -c），对齐 skill hooks.cpp 的既有用法
#if defined(_WIN32)
    auto res = exec("cmd.exe", ExecOptions{
        .cwd = cwd,
        .args = {"/d", "/s", "/c", cmd},
        .timeout = std::chrono::milliseconds(60000),
    });
#else
    auto res = exec("sh", ExecOptions{
        .cwd = cwd,
        .args = {"-c", cmd},
        .timeout = std::chrono::milliseconds(60000),
    });
#endif
    if (res.is_err()) {
        LOG_WARN("[verdict] exec failed cmd='{}': {}", cmd, res.error().message);
        return -1;
    }
    return res.value().exit_code;
}

/// @brief 默认测试命令（可被 goal.command 覆盖）
const char* kTestCmd = "ctest --output-on-failure";
/// 默认编译（Windows 用 cmake --build；非 Windows 同，CMake 跨平台）
const char* kBuildCmd = "cmake --build . --config Debug";

Verdict checker_tests(const AgentGoal& goal, const std::string& cwd) {
    const std::string cmd = goal.command.empty() ? kTestCmd : goal.command;
    const int code = run_exit_code(cmd, cwd);
    if (code < 0) {
        return {GoalStatus::Failed, std::format("test command failed to start: {}", cmd)};
    }
    if (code == 0) {
        return {GoalStatus::Achieved, "all tests pass"};
    }
    return {GoalStatus::Pending, std::format("tests failing (exit={})", code)};
}

Verdict checker_build(const AgentGoal& goal, const std::string& cwd) {
    const std::string cmd = goal.command.empty() ? kBuildCmd : goal.command;
    const int code = run_exit_code(cmd, cwd);
    if (code < 0) {
        return {GoalStatus::Failed, std::format("build command failed to start: {}", cmd)};
    }
    if (code == 0) {
        return {GoalStatus::Achieved, "build clean (zero errors)"};
    }
    return {GoalStatus::Pending, std::format("build has errors (exit={})", code)};
}

Verdict checker_lint(const AgentGoal& goal, const std::string& cwd) {
    const std::string cmd = goal.command.empty() ? "echo 'no lint config'; exit 0" : goal.command;
    const int code = run_exit_code(cmd, cwd);
    if (code < 0) {
        return {GoalStatus::Failed, std::format("lint command failed to start: {}", cmd)};
    }
    if (code == 0) {
        return {GoalStatus::Achieved, "lint zero warnings"};
    }
    return {GoalStatus::Pending, std::format("lint has warnings (exit={})", code)};
}

Verdict checker_file_exists(const AgentGoal& goal, const std::string& /*cwd*/) {
    if (goal.path.empty()) {
        return {GoalStatus::Failed, "file_exists goal requires path"};
    }
    std::error_code ec;
    const bool ok = std::filesystem::exists(goal.path, ec);
    if (ec) {
        return {GoalStatus::Failed, std::format("cannot stat '{}': {}", goal.path, ec.message())};
    }
    if (ok) {
        return {GoalStatus::Achieved, std::format("file exists: {}", goal.path)};
    }
    return {GoalStatus::Pending, std::format("file missing: {}", goal.path)};
}

Verdict checker_custom(const AgentGoal& goal, const std::string& cwd) {
    if (goal.command.empty()) {
        return {GoalStatus::Failed, "custom_script goal requires command"};
    }
    const int code = run_exit_code(goal.command, cwd);
    if (code < 0) {
        return {GoalStatus::Failed, std::format("custom script failed to start: {}", goal.command)};
    }
    if (code == 0) {
        return {GoalStatus::Achieved, "custom script exited 0"};
    }
    return {GoalStatus::Pending, std::format("custom script exited {}", code)};
}

Verdict check_goal(const AgentGoal& goal, const std::string& cwd) {
    switch (goal.type) {
        case AgentGoal::TestsPass:    return checker_tests(goal, cwd);
        case AgentGoal::BuildClean:   return checker_build(goal, cwd);
        case AgentGoal::LintZero:     return checker_lint(goal, cwd);
        case AgentGoal::FileExists:   return checker_file_exists(goal, cwd);
        case AgentGoal::CustomScript: return checker_custom(goal, cwd);
        case AgentGoal::None:
        default:
            return {GoalStatus::Pending, "no goal"};
    }
}

bool has_checker(AgentGoal::Type type) noexcept {
    switch (type) {
        case AgentGoal::TestsPass:
        case AgentGoal::BuildClean:
        case AgentGoal::LintZero:
        case AgentGoal::FileExists:
        case AgentGoal::CustomScript:
            return true;
        case AgentGoal::None:
        default:
            return false;
    }
}

namespace {

/// @brief 前缀匹配（按子串前缀，如 "file_exists:" 与 "cmd:" 需要冒号分隔）
bool starts_with(std::string_view v, std::string_view prefix) noexcept {
    return v.size() >= prefix.size() && v.substr(0, prefix.size()) == prefix;
}

} // namespace

AgentGoal parse_goal(std::string_view spec) noexcept {
    const std::string v = trimmed_lower(spec);
    AgentGoal goal;
    if (v.empty()) {
        return goal;  // None
    }
    if (v == "tests_pass" || v == "tests" || v == "test" || v == "ctest") {
        goal.type = AgentGoal::TestsPass;
        return goal;
    }
    if (v == "build_clean" || v == "build" || v == "compile") {
        goal.type = AgentGoal::BuildClean;
        return goal;
    }
    if (v == "lint_zero" || v == "lint") {
        goal.type = AgentGoal::LintZero;
        return goal;
    }
    if (starts_with(v, "file_exists:")) {
        goal.type = AgentGoal::FileExists;
        goal.path = v.substr(std::string_view("file_exists:").size());
        return goal;
    }
    if (starts_with(v, "cmd:")) {
        goal.type = AgentGoal::CustomScript;
        goal.command = v.substr(std::string_view("cmd:").size());
        return goal;
    }
    // 无法识别 → None（不抛异常，避免配置错误崩会话）
    return goal;
}

} // namespace agent