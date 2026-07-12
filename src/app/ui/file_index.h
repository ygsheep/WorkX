/**
 * @file file_index.h
 * @brief FileIndex — 文件索引与搜索
 * @details TUI 启动时扫描工作目录构建文件索引，支持按文件名搜索和补全
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <mutex>

namespace agent {

/// @brief FileIndex — 文件索引与搜索
///
/// TUI 启动时扫描工作目录（BFS）构建文件索引：
/// - 支持按文件名子串搜索（大小写不敏感）
/// - 空查询返回最近修改的 15 个文件
/// - 非空查询返回名称包含查询串的文件（最多 50 个）
/// - 自动跳过 .git/build/node_modules 等目录
class FileIndex {
public:
    /// @brief 索引条目
    struct Entry {
        std::string name;                   ///< 文件名（如 "main.cpp"）
        std::string relative_path;          ///< 相对路径（如 "src/app/main.cpp"）
        std::filesystem::file_time_type modified; ///< 修改时间
        bool is_directory{false};           ///< 是否为目录
    };

    /// @brief 默认空查询返回的文件数
    static constexpr size_t DEFAULT_EMPTY_LIMIT = 15;

    /// @brief 默认搜索查询返回的文件数
    static constexpr size_t DEFAULT_SEARCH_LIMIT = 50;

    /// @brief 构建文件索引（BFS 遍历）
    /// @param cwd 工作目录
    /// @param max_files 最大索引文件数（默认 10000）
    void build(const std::string& cwd, size_t max_files = 10000);

    /// @brief 搜索文件
    /// @param query 搜索查询（空字符串 → 返回最近修改的文件）
    /// @param limit 最大返回数
    /// @return 匹配的文件条目列表（按修改时间倒序）
    std::vector<Entry> search(
        const std::string& query,
        size_t limit = DEFAULT_SEARCH_LIMIT
    ) const;

    /// @brief 搜索文件名（仅返回路径字符串）
    /// @param query 搜索查询
    /// @param limit 最大返回数
    /// @return 匹配的文件路径列表
    std::vector<std::string> search_paths(
        const std::string& query,
        size_t limit = DEFAULT_SEARCH_LIMIT
    ) const;

    /// @brief 索引是否已就绪
    /// @return 已构建返回 true
    bool is_ready() const;

    /// @brief 获取索引文件数
    /// @return 文件数量
    size_t size() const;

private:
    std::vector<Entry> entries_;
    bool ready_{false};
    mutable std::mutex mutex_;

    /// @brief 判断目录是否应跳过
    /// @param dir_name 目录名
    /// @return 应跳过返回 true
    static bool should_skip_dir(const std::string& dir_name);
};

/// @brief 全局 FileIndex 实例
/// @return FileIndex 单例引用
FileIndex& global_file_index();

} // namespace agent
