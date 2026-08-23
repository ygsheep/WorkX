#include "agent/core/mode_agent_common.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <string>

#include "agent/core/verdict.h"                  // guard_command
#include "agent/tool/GlobTool/glob_tool.h"       // tool::glob_match
#include "core/process/subprocess.h"             // process::exec / ExecOptions

namespace fs = std::filesystem;

namespace agent {

namespace {

// 数组化替换字符串中的全部 {item} 占位为子串 repl
std::string replace_all(std::string s, const std::string& needle,
                        const std::string& repl) {
    if (needle.empty()) {
        return s;
    }
    size_t pos = 0;
    while ((pos = s.find(needle, pos)) != std::string::npos) {
        s.replace(pos, needle.size(), repl);
        pos += repl.size();
    }
    return s;
}

// 命令字符串是否包含需 shell 引用的敏感字符（注入面）
bool has_shell_meta(char c) noexcept {
    switch (c) {
        case '"': case '\'': case '`': case ';': case '&': case '|':
        case '<': case '>': case '$': case '(': case ')': case '\n':
        case '\r': case '\t': case '*': case '?':
            return true;
        default:
            return false;
    }
}

} // namespace

BatchSpec parse_batch_spec(const AgentGoal& goal) noexcept {
    BatchSpec spec;
    spec.glob = goal.glob.empty() ? "**/*" : goal.glob;
    spec.cmd_template = goal.command;
    spec.concurrency = static_cast<size_t>(std::max(1, goal.concurrency));
    return spec;
}

WatchSpec parse_watch_spec(const AgentGoal& goal) noexcept {
    WatchSpec spec;
    spec.path = goal.path.empty() ? "." : goal.path;
    spec.glob = goal.glob.empty() ? "" : goal.glob;
    spec.cmd_template = goal.command;
    spec.max_polls = std::max(1, goal.watch_polls);
    spec.interval_ms = std::max(0, goal.watch_interval_ms);
    return spec;
}

std::string materialize_cmd(const std::string& tmpl,
                            const std::string& item) noexcept {
    // 拒绝含 shell 敏感字符的 item（文件路径不应含这些；含则按"无法安全引用"跳过）
    if (std::any_of(item.begin(), item.end(),
                    [](char c) { return has_shell_meta(c); })) {
        return {};
    }
    // 平台 shell 引用：cmd.exe 用双引号、POSIX 用单引号（item 已保证不含对应引号）
    std::string quoted =
#ifdef _WIN32
        "\"" + item + "\"";
#else
        "'" + item + "'";
#endif
    return replace_all(tmpl, "{item}", quoted);
}

std::vector<std::string> expand_glob_cwd(const std::string& cwd,
                                         const std::string& pattern,
                                         std::string* err) {
    std::vector<std::string> result;
    if (err) {
        err->clear();
    }
    std::string p = pattern;
    std::replace(p.begin(), p.end(), '\\', '/');
    if (p.empty()) {
        p = "**/*";
    }

    std::error_code ec;
    const fs::path root(cwd.empty() ? fs::current_path() : fs::path(cwd));
    if (!fs::is_directory(root, ec)) {
        if (err) {
            *err = std::string("not a directory: ") + root.string();
        }
        return result;
    }
    for (auto it = fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        const auto& entry = *it;
        std::string rel = (entry.path().lexically_relative(root)).generic_string();
        if (rel.empty() || rel == ".") {
            continue;
        }
        if (tool::glob_match(p, rel)) {
            result.push_back(rel);
        }
    }
    std::sort(result.begin(), result.end());
    if (err && result.empty() && !fs::exists(root, ec)) {
        *err = std::string("search root missing: ") + root.string();
    }
    return result;
}

std::string snapshot_signature(const std::string& cwd,
                               const std::vector<std::string>& rels) {
    const fs::path root(cwd.empty() ? fs::current_path() : fs::path(cwd));
    std::error_code ec;
    std::string sig;
    for (const auto& rel : rels) {
        const fs::path p = (root / rel).lexically_normal();
        if (!fs::is_regular_file(p, ec)) {
            ec.clear();
            continue;
        }
        const std::uintmax_t sz = fs::file_size(p, ec);
        if (ec) {
            continue;  // file_size 出错（权限/竞态）→ 该文件跳过，避免签名抖动
        }
        const auto t = fs::last_write_time(p, ec);
        if (ec) {
            continue;
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            t.time_since_epoch()).count();
        sig += rel;
        sig += '|';
        sig += std::to_string(sz);
        sig += '|';
        sig += std::to_string(ms);
        sig += '\n';
    }
    return sig;
}

process::ExecOutput run_whitelisted(const std::string& cmd,
                                    const std::string& cwd,
                                    bool* rejected) {
    if (rejected) {
        *rejected = false;
    }
    // 白名单拦截：返回默认 ExecOutput（exit_code=-1），调用方据 rejected 处理
    if (guard_command(cmd).empty()) {
        if (rejected) {
            *rejected = true;
        }
        return {};
    }
    using namespace agent::process;
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
        ExecOutput out;
        out.exit_code = -1;
        out.stderr_text = res.error().message;
        out.cancelled = true;
        return out;
    }
    return res.value();
}

} // namespace agent