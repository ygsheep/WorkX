/**
 * @file session_restore.cpp
 * @brief 项目会话恢复：启动时询问用户实现
 * @version 1.0.0
 * @date 2026-07
 */

#include "app/session_restore.h"

#include <algorithm>
#include <iostream>
#include <string>

#include "agent/session/session_store.h"

namespace agent {

std::optional<std::string> prompt_restore_session(const std::string& project_dir) {
    auto sessions = session::SessionStore::list_sessions(project_dir);

    if (sessions.empty()) {
        return std::nullopt;  // 无历史会话，直接开新会话
    }

    // 最多展示最近 5 条
    const int show_count = static_cast<int>(std::min<size_t>(5, sessions.size()));

    std::cout << "\n";
    std::cout << "========================================\n";
    std::cout << "  发现 " << sessions.size() << " 个历史会话，是否恢复？\n";
    std::cout << "========================================\n";
    std::cout << "\n";

    for (int i = 0; i < show_count; ++i) {
        const auto& s = sessions[i];
        // 显示格式：[编号] 创建时间 | git分支 | 消息数 | 模型
        std::string branch = s.git_branch.empty() ? "no-branch" : s.git_branch;
        std::string created = s.created_at.empty() ? "unknown" : s.created_at;
        std::string model = s.model.empty() ? "unknown" : s.model;
        std::cout << "  [" << (i + 1) << "] " << created
                  << " | " << branch
                  << " | " << s.message_count << " 条消息"
                  << " | " << model << "\n";
    }

    std::cout << "\n";
    std::cout << "  [n] 开始新会话\n";
    std::cout << "\n";
    std::cout << "请选择 (1-" << show_count << " 或 n): ";

    std::string input;
    if (!std::getline(std::cin, input)) {
        return std::nullopt;  // 输入失败（如管道关闭），默认开新会话
    }

    // 去除首尾空白
    while (!input.empty() && (input.front() == ' ' || input.front() == '\t')) {
        input.erase(input.begin());
    }
    while (!input.empty() && (input.back() == ' ' || input.back() == '\t' ||
                              input.back() == '\r' || input.back() == '\n')) {
        input.pop_back();
    }

    // 空输入或 n/N → 开新会话
    if (input.empty() || input == "n" || input == "N") {
        return std::nullopt;
    }

    // 尝试解析数字
    try {
        int choice = std::stoi(input);
        if (choice >= 1 && choice <= show_count) {
            return sessions[choice - 1].file_path;
        }
    } catch (...) {
        // 无效输入，默认开新会话
    }

    return std::nullopt;
}

} // namespace agent
