/**
 * @file bash_tool.cpp
 * @brief BashTool 实现
 * @details Shell 执行工具的具体实现，支持同步/后台/沙盒/超时/进度回调
 * @version 1.1.0
 * @date 2026-07
 */

#include "agent/tool/BashTool/bash_tool.h"

#include "core/process/subprocess.h"
#include "core/process/exec_output.h"
#include "core/process/sandbox/sandbox_adapter.h"
#include "core/process/sandbox/sandbox_config.h"
#include "core/process/sandbox/sandbox_detector.h"
#include "core/task/task_manager.h"
#include "core/utils/error.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>

namespace agent::tool {

namespace {

/// 默认超时：120 秒（对齐 cc BashTool 默认值）
constexpr int kDefaultTimeoutMs = 120'000;
/// 最大超时上限：600 秒（对齐 cc getMaxTimeoutMs）
constexpr int kMaxTimeoutMs = 600'000;
/// 输出截断阈值（对齐 ToolExecutor::MAX_TOOL_RESULT_LENGTH）
constexpr size_t kMaxOutputChars = 8'000;
/// 后台任务输出注册表容量上限（防止内存膨胀）
constexpr size_t kMaxRegistryEntries = 50;
/// 后台任务名唯一 id 计数器（M-3：避免 task_name 重复）
std::atomic<size_t> s_task_id_counter{0};

/// @brief 后台任务输出注册表（M-1 修复）
/// @details 保存后台任务的 ExecOutput，供 LLM 通过未来 GetBashOutputTool 查询。
///          线程安全，容量上限 kMaxRegistryEntries，FIFO 淘汰最旧条目。
class BashOutputRegistry {
public:
    static BashOutputRegistry& instance() {
        static BashOutputRegistry inst;
        return inst;
    }

    void store(const std::string& task_name, const process::ExecOutput& out) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_outputs[task_name] = out;
        // 超容量时淘汰最旧条目（按插入顺序，std::map 按键排序不够精确，
        // 但 task_name 含递增 id，按键排序近似 FIFO）
        if (m_outputs.size() > kMaxRegistryEntries) {
            m_outputs.erase(m_outputs.begin());
        }
    }

    /// 查询并返回输出副本；不存在返回空 optional
    std::optional<process::ExecOutput> get(const std::string& task_name) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_outputs.find(task_name);
        if (it == m_outputs.end()) return std::nullopt;
        return it->second;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_outputs.clear();
    }

private:
    BashOutputRegistry() = default;
    mutable std::mutex m_mutex;
    std::map<std::string, process::ExecOutput> m_outputs;
};

/// 跨平台 shell 选择
#ifdef _WIN32
constexpr const char* kShell = "cmd.exe";
constexpr const char* kShellFlag = "/c";
#else
constexpr const char* kShell = "/bin/sh";
constexpr const char* kShellFlag = "-c";
#endif

/// @brief 截断输出到指定字符数，保留头尾
/// @details 对齐 ToolExecutor::finalize_result 的截断逻辑
std::string truncate_output(std::string s, size_t max_chars) {
    if (s.size() <= max_chars) return s;
    const size_t head = max_chars / 2;
    const size_t tail = max_chars - head;
    const size_t omitted = s.size() - max_chars;
    return s.substr(0, head) +
           std::format("\n... [output truncated, {} characters omitted] ...\n", omitted) +
           s.substr(s.size() - tail);
}

/// @brief 去除连续空行（对齐 cc stripEmptyLines）
/// @details L-1 修复：末尾仅保留单个换行，避免 </stdout> 前出现空行
std::string strip_empty_lines(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool last_was_blank = false;
    std::istringstream iss(s);
    std::string line;
    while (std::getline(iss, line)) {
        bool blank = line.find_first_not_of(" \t\r") == std::string::npos;
        if (blank && last_was_blank) continue;
        out += line;
        out += '\n';
        last_was_blank = blank;
    }
    // L-1：去掉末尾多余的连续换行，只保留一个
    while (out.size() >= 2 && out.back() == '\n' && out[out.size() - 2] == '\n') {
        out.pop_back();
    }
    return out;
}

/// @brief 格式化执行结果为 LLM 可读文本
std::string format_result(const process::ExecOutput& out) {
    std::ostringstream ss;
    if (out.timed_out) {
        ss << "<error>Command timed out</error>\n";
    } else if (out.cancelled) {
        ss << "<error>Command was cancelled</error>\n";
    } else if (out.exit_code != 0) {
        ss << std::format("<error>Command exited with code {}</error>\n", out.exit_code);
    }

    if (!out.stdout_text.empty()) {
        ss << "<stdout>\n" << strip_empty_lines(out.stdout_text) << "</stdout>\n";
    }
    if (!out.stderr_text.empty()) {
        ss << "<stderr>\n" << strip_empty_lines(out.stderr_text) << "</stderr>\n";
    }
    if (out.stdout_text.empty() && out.stderr_text.empty()) {
        ss << "(no output)\n";
    }
    return ss.str();
}

} // namespace

// ============================================================
// 元信息
// ============================================================

const std::string& BashTool::name() const {
    static const std::string n{"Bash"};
    return n;
}

const std::string& BashTool::description() const {
    static const std::string d{
        "Executes a shell command and returns stdout, stderr, and exit code. "
        "Supports timeout, working directory, and optional background execution."
    };
    return d;
}

const std::string& BashTool::prompt() const {
    static const std::string p{
        "Executes a shell command on the user's machine.\n\n"
        "## When to use\n"
        "- Running tests, builds, or scripts\n"
        "- Inspecting files with system tools (grep, find, ls)\n"
        "- Git operations\n"
        "- Any task requiring shell access\n\n"
        "## Guidelines\n"
        "- Prefer specialized tools (FileRead/FileEdit/FileWrite/Grep/Glob) when applicable\n"
        "- Use `description` to briefly explain what the command does (5-10 words)\n"
        "- Set `timeout` for long-running commands (max 600000ms)\n"
        "- Use `run_in_background=true` for long-running commands whose output "
        "you don't immediately need (e.g. starting a dev server)\n"
        "- Avoid interactive commands that require user input\n"
        "- Commands run in the current working directory by default\n"
        "- The sandbox restricts filesystem and network access by default; "
        "set `dangerously_disable_sandbox=true` only when the command truly "
        "needs unrestricted access\n\n"
        "## Output format\n"
        "- stdout is wrapped in <stdout>...</stdout> tags\n"
        "- stderr is wrapped in <stderr>...</stderr> tags\n"
        "- Non-zero exit codes are reported in <error>...</error> tags\n"
        "- Output exceeding 8000 characters is truncated\n"
    };
    return p;
}

nlohmann::json BashTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties", {
            {"command", {
                {"type", "string"},
                {"description", "The shell command to execute"}
            }},
            {"description", {
                {"type", "string"},
                {"description", "Brief description of what the command does (5-10 words)"}
            }},
            {"timeout", {
                {"type", "integer"},
                {"description", "Timeout in milliseconds (max 600000)"},
                {"default", 120000}
            }},
            {"cwd", {
                {"type", "string"},
                {"description", "Working directory for the command (defaults to ctx.cwd). Use absolute paths."}
            }},
            {"run_in_background", {
                {"type", "boolean"},
                {"description", "Run command in background, return immediately with task id"},
                {"default", false}
            }},
            {"dangerously_disable_sandbox", {
                {"type", "boolean"},
                {"description", "Disable sandbox restrictions for this command"},
                {"default", false}
            }}
        }},
        {"required", {"command"}},
        {"additionalProperties", false}
    };
}

// ============================================================
// call() — 入口分发
// ============================================================

ResultV2<ToolResult> BashTool::call(
    const nlohmann::json& input,
    const ToolContext& ctx
) const {
    // 解析参数
    if (!input.contains("command") || !input["command"].is_string()) {
        return ResultV2<ToolResult>::err(
            Error::Code::MissingArgument, "BashTool: 'command' is required");
    }
    const std::string command = input["command"].get<std::string>();
    if (command.empty()) {
        return ResultV2<ToolResult>::err(
            Error::Code::InvalidInput, "BashTool: 'command' must not be empty");
    }

    int timeout_ms = kDefaultTimeoutMs;
    if (input.contains("timeout") && input["timeout"].is_number()) {
        timeout_ms = input["timeout"].get<int>();
        if (timeout_ms <= 0) timeout_ms = kDefaultTimeoutMs;
        if (timeout_ms > kMaxTimeoutMs) timeout_ms = kMaxTimeoutMs;
    }

    bool run_in_background = false;
    if (input.contains("run_in_background") && input["run_in_background"].is_boolean()) {
        run_in_background = input["run_in_background"].get<bool>();
    }

    bool disable_sandbox = false;
    if (input.contains("dangerously_disable_sandbox") &&
        input["dangerously_disable_sandbox"].is_boolean()) {
        disable_sandbox = input["dangerously_disable_sandbox"].get<bool>();
    }

    // cwd：优先用参数，否则用 ctx.cwd
    std::string cwd = ctx.cwd;
    if (input.contains("cwd") && input["cwd"].is_string() && !input["cwd"].get<std::string>().empty()) {
        cwd = input["cwd"].get<std::string>();
    }

    // 取消检查
    if (ctx.is_cancelled()) {
        return ResultV2<ToolResult>::err(
            Error::Code::Cancelled, "BashTool: cancelled before execution");
    }

    if (run_in_background) {
        return execute_background(command, cwd, timeout_ms, disable_sandbox, ctx);
    }
    return execute_sync(command, cwd, timeout_ms, disable_sandbox, ctx);
}

// ============================================================
// execute_sync — 同步执行路径
// ============================================================

ResultV2<ToolResult> BashTool::execute_sync(
    const std::string& command,
    const std::string& cwd,
    int timeout_ms,
    bool disable_sandbox,
    const ToolContext& ctx
) const {
    // 上报开始
    ctx.report_progress(std::format("Executing: {}", command));

    // 构建沙盒配置（若启用）
    process::sandbox::SandboxConfig sb_config = disable_sandbox
        ? process::sandbox::SandboxConfig::permissive()
        : process::sandbox::SandboxConfig::restrictive(cwd);

    // 包装命令：通过 shell 执行，使管道/重定向/复合命令可用
    auto wrapped = process::sandbox::SandboxAdapter::wrap_command(
        kShell, {kShellFlag, command}, sb_config);

    // 上报降级或包装情况
    if (wrapped.was_wrapped) {
        std::string sb_status = wrapped.degraded ? "degraded" : "active";
        ctx.report_progress("Sandbox: " + sb_status + " (backend: " + wrapped.backend_name + ")");
    }

    // 构建 ExecOptions
    process::ExecOptions opts;
    opts.cwd = cwd;
    opts.args = wrapped.args;
    if (timeout_ms > 0) {
        opts.timeout = std::chrono::milliseconds(timeout_ms);
    }
    // 取消回调：绑定到 ctx
    if (ctx.cancel_flag != nullptr) {
        const std::atomic<bool>* flag = ctx.cancel_flag;
        opts.is_cancelled = [flag]() {
            return flag->load(std::memory_order_acquire);
        };
    }

    // 执行
    auto exec_result = process::exec(wrapped.cmd, opts);
    if (exec_result.is_err()) {
        const auto& err = exec_result.error();
        // 启动失败（命令不存在等）
        std::string msg = std::format("Failed to execute command: {}", err.message);
        return ResultV2<ToolResult>::err(err.code, std::move(msg));
    }

    const auto& out = exec_result.value();
    std::string formatted = format_result(out);
    formatted = truncate_output(std::move(formatted), kMaxOutputChars);

    // 上报完成
    if (out.is_success()) {
        ctx.report_progress("Command completed successfully");
    } else if (out.timed_out) {
        ctx.report_progress("Command timed out");
    } else if (out.cancelled) {
        ctx.report_progress("Command cancelled");
    } else {
        ctx.report_progress("Command exited with code " + std::to_string(out.exit_code));
    }

    return ResultV2<ToolResult>::ok(ToolResult::ok(std::move(formatted)));
}

// ============================================================
// execute_background — 后台执行路径
// ============================================================

ResultV2<ToolResult> BashTool::execute_background(
    const std::string& command,
    const std::string& cwd,
    int timeout_ms,
    bool disable_sandbox,
    const ToolContext& ctx
) const {
    // 检查 TaskManager 是否可用
    if (ctx.task_manager_ptr == nullptr) {
        return ResultV2<ToolResult>::err(
            Error::Code::ConfigInvalid,
            "BashTool: run_in_background=true requires TaskManager "
            "(ctx.task_manager_ptr is null)");
    }

    // 沙盒配置
    process::sandbox::SandboxConfig sb_config = disable_sandbox
        ? process::sandbox::SandboxConfig::permissive()
        : process::sandbox::SandboxConfig::restrictive(cwd);
    auto wrapped = process::sandbox::SandboxAdapter::wrap_command(
        kShell, {kShellFlag, command}, sb_config);

    // M-3 修复：task_name 拼接唯一递增 id，避免不同任务重名
    // 格式：bash:<id>:<command 前 40 字符>
    const size_t task_id = s_task_id_counter.fetch_add(1, std::memory_order_relaxed);
    std::string task_name = "bash:" + std::to_string(task_id) + ":" + command.substr(0, 40);

    auto& tm = ctx.task_manager();
    auto task = tm.launch(task_name,
        [command, cwd, timeout_ms, wrapped, task_name](const std::atomic<bool>& should_cancel) {
            process::ExecOptions opts;
            opts.cwd = cwd;
            opts.args = wrapped.args;
            if (timeout_ms > 0) {
                opts.timeout = std::chrono::milliseconds(timeout_ms);
            }
            opts.is_cancelled = [&should_cancel]() {
                return should_cancel.load(std::memory_order_acquire);
            };

            auto exec_result = process::exec(wrapped.cmd, opts);
            // M-1 修复：
            // 1. exec 启动失败（is_err）抛异常 → TaskManager 标记 Failed（发布 TaskFailedEvent）
            //    原实现 (void)exec_result 会导致 TaskManager 错误地标记 Completed
            if (exec_result.is_err()) {
                const auto& err = exec_result.error();
                throw std::runtime_error(
                    "Failed to execute command: " + err.message);
            }
            // 2. exec 成功（即使非零退出/超时/取消）→ 保存输出到注册表，供 LLM 查询
            //    非零退出/超时不视为 task 失败（命令执行了，只是结果非成功），符合 cc 行为
            BashOutputRegistry::instance().store(task_name, exec_result.value());
        },
        TaskType::Background);

    if (!task) {
        return ResultV2<ToolResult>::err(
            Error::Code::InternalError,
            "BashTool: failed to launch background task");
    }

    // 立即返回 task 信息
    std::string task_name_str = task->getName();
    std::string result = "Command running in background with ID: " + task_name_str +
        "\nCommand: " + command +
        "\nOutput will be available when the task completes.";

    ctx.report_progress("Background task started: " + task_name_str);

    return ResultV2<ToolResult>::ok(ToolResult::ok(std::move(result)));
}

} // namespace agent::tool
