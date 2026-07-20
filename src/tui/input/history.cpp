/**
 * @file history.cpp
 * @brief 输入历史管理实现
 * @version 2.0.0
 */

#include "tui/input/history.h"

#include <fstream>
#include <algorithm>

namespace agent {

History::History(size_t max_entries)
    : m_max_entries(max_entries)
{
}

void History::add(std::string_view entry) {
    if (entry.empty()) return;
    if (!m_entries.empty() && m_entries.back() == entry) return;
    m_entries.emplace_back(entry);

    // 淘汰最旧的条目
    while (m_entries.size() > m_max_entries) {
        m_entries.erase(m_entries.begin());
    }
}

bool History::load(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || ec) {
        return false;
    }

    std::ifstream file(path);
    if (!file.is_open()) return false;

    std::string line;
    while (std::getline(file, line)) {
        if (!line.empty()) {
            m_entries.push_back(std::move(line));
        }
    }

    // 超限时截断
    if (m_entries.size() > m_max_entries) {
        size_t excess = m_entries.size() - m_max_entries;
        m_entries.erase(m_entries.begin(), m_entries.begin() + static_cast<ptrdiff_t>(excess));
    }

    return true;
}

bool History::save(const std::filesystem::path& path) const {
    // 确保父目录存在
    std::error_code ec;
    auto parent = path.parent_path();
    if (!parent.empty() && !std::filesystem::exists(parent, ec)) {
        std::filesystem::create_directories(parent, ec);
        if (ec) return false;
    }

    std::ofstream file(path);
    if (!file.is_open()) return false;

    for (const auto& entry : m_entries) {
        file << entry << '\n';
    }

    return file.good();
}

} // namespace agent
