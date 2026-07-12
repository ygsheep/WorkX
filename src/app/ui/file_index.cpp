/**
 * @file file_index.cpp
 * @brief FileIndex 实现
 * @details 文件索引构建（BFS）与搜索实现
 * @version 1.0.0
 * @date 2026-07
 */

#include "app/ui/file_index.h"

#include <algorithm>
#include <cctype>
#include <queue>
#include <set>

namespace agent {

namespace fs = std::filesystem;

// ============================================================
// 私有辅助
// ============================================================

bool FileIndex::should_skip_dir(const std::string& dir_name) {
    // 常见需要跳过的目录（含隐藏目录中的无关项）
    static const std::set<std::string> skip_dirs = {
        ".git",          ///< Git 元数据（大量无关文件）
        ".vs",           ///< Visual Studio 临时文件
        ".vscode",       ///< VS Code 配置（可选，但通常无需搜索）
        ".idea",         ///< JetBrains 配置
        ".cache",        ///< 缓存目录
        "build", "out", "bin", "obj",
        "node_modules", "__pycache__", ".venv", "venv",
        "vcpkg_installed", "CMakeFiles",
        "cmake-build-debug", "cmake-build-release",
        "dist", "target", "Debug", "Release",
        "x64", "Win32", "ARM", "ARM64",
    };

    return skip_dirs.count(dir_name) > 0;
}

/// @brief 小写转换
static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
        [](unsigned char c) { return std::tolower(c); });
    return s;
}

// ============================================================
// 构建索引
// ============================================================

void FileIndex::build(const std::string& cwd, size_t max_files) {
    std::lock_guard<std::mutex> lock(mutex_);

    entries_.clear();
    entries_.reserve(std::min(max_files, static_cast<size_t>(1000)));

    fs::path root(cwd);
    std::error_code ec;

    if (!fs::is_directory(root, ec)) {
        ready_ = false;
        return;
    }

    // BFS 遍历
    std::queue<fs::path> dir_queue;
    dir_queue.push(root);

    while (!dir_queue.empty() && entries_.size() < max_files) {
        fs::path current_dir = dir_queue.front();
        dir_queue.pop();

        for (auto it = fs::directory_iterator(
                 current_dir,
                 fs::directory_options::skip_permission_denied,
                 ec);
             it != fs::directory_iterator();
             it.increment(ec)) {

            if (ec) {
                ec.clear();
                continue;
            }

            if (entries_.size() >= max_files) {
                break;
            }

            const auto& entry = *it;
            std::string name = entry.path().filename().string();

            if (entry.is_directory(ec)) {
                // 跳过特定目录（如 .git/build/node_modules）
                if (!should_skip_dir(name)) {
                    dir_queue.push(entry.path());

                    // 目录也加入索引
                    Entry e;
                    e.name = name;
                    e.relative_path = fs::relative(entry.path(), root, ec).string();
                    if (ec) {
                        ec.clear();
                        continue;
                    }
                    std::replace(e.relative_path.begin(), e.relative_path.end(), '\\', '/');
                    e.modified = entry.last_write_time(ec);
                    if (ec) {
                        ec.clear();
                        e.modified = fs::file_time_type{};
                    }
                    e.is_directory = true;
                    entries_.push_back(std::move(e));
                }
            } else if (entry.is_regular_file(ec)) {
                Entry e;
                e.name = name;
                e.relative_path = fs::relative(entry.path(), root, ec).string();
                if (ec) {
                    ec.clear();
                    continue;
                }

                // 规范化路径分隔符
                std::replace(e.relative_path.begin(), e.relative_path.end(), '\\', '/');

                e.modified = entry.last_write_time(ec);
                if (ec) {
                    ec.clear();
                    e.modified = fs::file_time_type{};
                }

                entries_.push_back(std::move(e));
            }
        }
    }

    // 按修改时间倒序排列（最新在前）
    std::sort(entries_.begin(), entries_.end(),
        [](const Entry& a, const Entry& b) {
            return a.modified > b.modified;
        });

    ready_ = true;
}

// ============================================================
// 搜索
// ============================================================

std::vector<FileIndex::Entry> FileIndex::search(
    const std::string& query,
    size_t limit
) const {
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<Entry> results;

    if (!ready_ || entries_.empty()) {
        return results;
    }

    // 空查询 → 返回最近修改的文件（最多 limit 个）
    if (query.empty()) {
        size_t count = std::min(limit, entries_.size());
        results.assign(entries_.begin(), entries_.begin() + count);
        return results;
    }

    // 非空查询 → 按文件名或路径子串匹配（大小写不敏感）
    std::string lower_query = to_lower(query);

    for (const auto& entry : entries_) {
        std::string lower_name = to_lower(entry.name);
        std::string lower_path = to_lower(entry.relative_path);

        if (lower_name.find(lower_query) != std::string::npos ||
            lower_path.find(lower_query) != std::string::npos) {
            results.push_back(entry);
            if (results.size() >= limit) {
                break;
            }
        }
    }

    return results;
}

std::vector<std::string> FileIndex::search_paths(
    const std::string& query,
    size_t limit
) const {
    auto entries = search(query, limit);
    std::vector<std::string> paths;
    paths.reserve(entries.size());
    for (auto& e : entries) {
        paths.push_back(std::move(e.relative_path));
    }
    return paths;
}

bool FileIndex::is_ready() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ready_;
}

size_t FileIndex::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

// ============================================================
// 全局单例
// ============================================================

FileIndex& global_file_index() {
    static FileIndex instance;
    return instance;
}

} // namespace agent
