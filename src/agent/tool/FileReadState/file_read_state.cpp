/**
 * @file file_read_state.cpp
 * @brief FileReadStateTracker 实现
 * @details 单例注册表的线程安全实现。所有方法通过 mutex 保护。
 * @author workx
 * @version 1.0.0
 * @date 2026-07
 */

#include "agent/tool/FileReadState/file_read_state.h"

namespace agent::tool {

// ============================================================
// 公共 API
// ============================================================

void FileReadStateTracker::record_read(
    const std::string& canonical_path,
    std::string content,
    std::filesystem::file_time_type mtime,
    bool is_partial_view
) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_states[canonical_path] = {
        .mtime = mtime,
        .content = std::move(content),
        .is_partial_view = is_partial_view
    };
}

std::optional<FileReadState> FileReadStateTracker::get_state(
    const std::string& canonical_path
) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_states.find(canonical_path);
    if (it == m_states.end()) {
        return std::nullopt;
    }
    return it->second;
}

void FileReadStateTracker::update_after_write(
    const std::string& canonical_path,
    std::string new_content,
    std::filesystem::file_time_type new_mtime,
    bool is_partial_view
) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_states[canonical_path] = {
        .mtime = new_mtime,
        .content = std::move(new_content),
        .is_partial_view = is_partial_view
    };
}

void FileReadStateTracker::remove(const std::string& canonical_path) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_states.erase(canonical_path);
}

void FileReadStateTracker::clear() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_states.clear();
}

void FileReadStateTracker::clear_for_test() {
    clear();
}

size_t FileReadStateTracker::size() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_states.size();
}

} // namespace agent::tool
