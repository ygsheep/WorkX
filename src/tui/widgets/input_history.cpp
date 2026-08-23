/**
 * @file input_history.cpp
 * @brief 输入历史持久化实现（JSON 数组格式，nlohmann_json 序列化）
 */

#include "widgets/input_history.h"

#include <fstream>

#include <nlohmann/json.hpp>

namespace ftxtui {

namespace {
/// @brief 历史条数上限（超出丢弃最旧）
constexpr size_t kMaxEntries = 200;
}  // namespace

void InputHistory::load(const std::filesystem::path& path) {
    m_path = path;
    m_entries.clear();
    m_pos = 0;
    m_draft.clear();
    std::ifstream in(path);
    if (!in.is_open()) return;
    try {
        const nlohmann::json j = nlohmann::json::parse(in);
        if (!j.is_array()) return;
        for (const auto& item : j) {
            if (!item.is_string()) continue;
            const std::string s = item.get<std::string>();
            if (!s.empty()) m_entries.push_back(s);
        }
    } catch (const nlohmann::json::exception&) {
        m_entries.clear();
        return;
    }
    m_pos = m_entries.size();
}

void InputHistory::save() const {
    if (m_path.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(m_path.parent_path(), ec);
    nlohmann::json j = nlohmann::json::array();
    for (const auto& s : m_entries) j.push_back(s);
    std::ofstream out(m_path);
    if (!out.is_open()) return;
    out << j.dump(2);
}

void InputHistory::push(const std::string& text) {
    if (text.empty()) return;
    if (!m_entries.empty() && m_entries.back() == text) return;  // 与最近一条相同则跳过
    m_entries.push_back(text);
    if (m_entries.size() > kMaxEntries)
        m_entries.erase(m_entries.begin(),
                        m_entries.begin() + static_cast<long>(m_entries.size() - kMaxEntries));
    m_pos = m_entries.size();  // 新提交后复位到草稿位
}

bool InputHistory::prev(const std::string& current, std::string& out) {
    if (m_entries.empty()) return false;
    if (m_pos == m_entries.size()) {
        m_draft = current;  // 首次离开草稿位：记录草稿
        m_pos = m_entries.size() - 1;
    } else if (m_pos == 0) {
        return false;  // 已到最旧
    } else {
        --m_pos;
    }
    out = m_entries[m_pos];
    return true;
}

bool InputHistory::next(std::string& out) {
    if (m_pos == m_entries.size()) return false;  // 已在草稿位
    ++m_pos;
    if (m_pos == m_entries.size()) {
        out = m_draft;  // 回到草稿位：还原草稿
        return true;
    }
    out = m_entries[m_pos];
    return true;
}

void InputHistory::reset_nav() { m_pos = m_entries.size(); }

}  // namespace ftxtui
