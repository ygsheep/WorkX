/**
 * @file glob_tool.cpp
 * @brief GlobTool 实现
 * @details 文件名匹配工具的具体实现：glob → regex 转换 + 递归遍历 + 按修改时间排序
 * @version 1.1.0
 * @date 2026-07
 */

#include "agent/tool/GlobTool/glob_tool.h"

#include <filesystem>
#include <regex>
#include <algorithm>
#include <vector>

namespace agent::tool {

namespace fs = std::filesystem;

// ============================================================
// 元数据方法
// ============================================================

const std::string& GlobTool::name() const {
    static const std::string n{"Glob"};
    return n;
}

const std::string& GlobTool::description() const {
    static const std::string d{"Finds files matching a glob pattern."};
    return d;
}

const std::string& GlobTool::prompt() const {
    static const std::string p{
        "Finds files matching a glob pattern. "
        "Supports ** (recursive), * (single level), and ? (single char) wildcards. "
        "Returns matching file paths sorted by modification time (newest first)."
    };
    return p;
}

nlohmann::json GlobTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties", {
            {"pattern", {{"type", "string"}, {"description", "The glob pattern to match files against"}}},
            {"cwd", {{"type", "string"}, {"description", "The directory to search in (defaults to context cwd)"}}}
        }},
        {"required", {"pattern"}}
    };
}

// ============================================================
// 输入验证
// ============================================================

ValidationResult GlobTool::validate_input(
    const nlohmann::json& input,
    const ToolContext& /*ctx*/
) const {
    if (!input.contains("pattern") || !input["pattern"].is_string()) {
        return ValidationResult::err("Missing required field: pattern");
    }
    if (input["pattern"].get<std::string>().empty()) {
        return ValidationResult::err("pattern must not be empty");
    }
    return ValidationResult::ok();
}

// ============================================================
// 私有辅助方法
// ============================================================

std::string GlobTool::normalize_path(const std::string& path) {
    std::string result = path;
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

std::string GlobTool::glob_to_regex(const std::string& glob) {
    std::string regex;
    regex.reserve(glob.size() * 2);
    regex += '^';

    for (size_t i = 0; i < glob.size(); ++i) {
        char c = glob[i];

        if (c == '*') {
            if (i + 1 < glob.size() && glob[i + 1] == '*') {
                // ** — 匹配任意层级（含路径分隔符）
                regex += ".*";
                ++i;
                // 跳过 ** 后的 /
                if (i + 1 < glob.size() && glob[i + 1] == '/') {
                    ++i;
                }
            } else {
                // * — 匹配单层文件名（不含路径分隔符）
                regex += "[^/]*";
            }
        } else if (c == '?') {
            // ? — 匹配单个字符（不含路径分隔符）
            regex += "[^/]";
        } else if (c == '.' || c == '+' || c == '(' || c == ')' ||
                   c == '^' || c == '$' || c == '|' || c == '\\') {
            // 转义正则特殊字符
            regex += '\\';
            regex += c;
        } else {
            regex += c;
        }
    }

    regex += '$';
    return regex;
}

// ============================================================
// 执行
// ============================================================

ToolResult GlobTool::call(
    const nlohmann::json& input,
    const ToolContext& ctx
) {
    // 1. 解析输入
    GlobInput glob_input = input.get<GlobInput>();

    // 2. 确定搜索目录
    std::string cwd = glob_input.cwd.empty() ? ctx.cwd : glob_input.cwd;
    if (cwd.empty()) {
        cwd = ".";
    }

    fs::path search_dir(cwd);
    std::error_code ec;
    if (!fs::exists(search_dir, ec)) {
        return ToolResult::error("Directory does not exist: " + cwd);
    }
    if (!fs::is_directory(search_dir, ec)) {
        return ToolResult::error("Path is not a directory: " + cwd);
    }

    // 3. 构建 glob 正则
    std::string normalized_pattern = normalize_path(glob_input.pattern);
    std::string regex_str = glob_to_regex(normalized_pattern);

    std::regex glob_re;
    try {
        glob_re = std::regex(regex_str);
    } catch (const std::regex_error& e) {
        return ToolResult::error(std::string("Invalid glob pattern: ") + e.what());
    }

    // 4. 递归遍历目录，收集匹配项
    struct MatchEntry {
        std::string relative_path;
        fs::file_time_type last_write_time;
    };

    std::vector<MatchEntry> matches;

    for (auto it = fs::recursive_directory_iterator(
             search_dir,
             fs::directory_options::skip_permission_denied,
             ec);
         it != fs::recursive_directory_iterator();
         it.increment(ec)) {

        if (ec) {
            ec.clear();
            continue;
        }

        const auto& entry = *it;

        // 获取相对路径并规范化
        std::string rel_path = normalize_path(
            fs::relative(entry.path(), search_dir, ec).string()
        );
        if (ec) {
            ec.clear();
            continue;
        }

        // 匹配 glob 正则
        if (std::regex_match(rel_path, glob_re)) {
            auto lwt = entry.last_write_time(ec);
            if (ec) {
                ec.clear();
                lwt = fs::file_time_type{};
            }
            matches.push_back({rel_path, lwt});
        }
    }

    // 5. 按修改时间倒序排列（最新优先）
    std::sort(matches.begin(), matches.end(),
        [](const MatchEntry& a, const MatchEntry& b) {
            return a.last_write_time > b.last_write_time;
        });

    // 6. 格式化输出
    if (matches.empty()) {
        return ToolResult::ok("No files matched pattern: " + glob_input.pattern);
    }

    std::string result;
    result.reserve(matches.size() * 40);
    for (const auto& m : matches) {
        result += m.relative_path;
        result += '\n';
    }

    // 移除末尾多余的换行
    if (!result.empty()) {
        result.pop_back();
    }

    return ToolResult::ok(std::move(result));
}

} // namespace agent::tool
