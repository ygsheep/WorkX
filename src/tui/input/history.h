/**
 * @file history.h
 * @brief 输入历史管理
 * @details 支持内存历史 + 文件持久化
 * @version 2.0.0
 */

#pragma once

#include <string>
#include <vector>
#include <filesystem>

namespace agent {

/**
 * @brief 输入历史管理器
 */
class History {
public:
    /// @brief 默认最大历史条目数
    static constexpr size_t DEFAULT_MAX_ENTRIES = 1000;

    /// @brief 构造
    /// @param max_entries 最大历史条目数（超过时淘汰最旧的）
    explicit History(size_t max_entries = DEFAULT_MAX_ENTRIES);

    /// @brief 添加条目（自动去重最后一条）
    void add(std::string_view entry);

    /// @brief 获取条目数
    size_t size() const { return m_entries.size(); }

    /// @brief 获取指定索引的条目
    const std::string& operator[](size_t idx) const { return m_entries[idx]; }

    /// @brief 清空历史
    void clear() { m_entries.clear(); }

    /// @brief 获取所有条目
    const std::vector<std::string>& entries() const { return m_entries; }

    /// @brief 从文件加载历史
    /// @param path 文件路径
    /// @return true 成功
    bool load(const std::filesystem::path& path);

    /// @brief 保存历史到文件
    /// @param path 文件路径
    /// @return true 成功
    bool save(const std::filesystem::path& path) const;

private:
    std::vector<std::string> m_entries;
    size_t m_max_entries;
};

} // namespace agent
