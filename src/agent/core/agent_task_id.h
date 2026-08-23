/**
 * @file agent_task_id.h
 * @brief Agent 任务 id 生成（子 Agent / 后台 Agent 共用，避免重复）
 * @details 对齐 TS generateTaskId：'a' 前缀 + 8 个随机小写字母数字。
 *          BackgroundAgent 复用前缀 'b'，避免与子 Agent 'a' 冲突。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <cstddef>
#include <mutex>
#include <random>
#include <string>

namespace agent {

/// @brief 生成 Agent 任务 id：prefix + 8 个随机小写字母数字（线程安全）
/// @param prefix 前缀（子 Agent='a'；后台 Agent='b'）
inline std::string generate_agent_task_id(char prefix) {
    static constexpr char kAlphabet[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    // L-2：std::random_device 与 uniform_int_distribution 均非线程安全，加锁保护
    //（inline 函数静态局部在全部 TU 间仅一份，线程安全）
    static std::mutex s_mutex;
    static std::random_device rd;
    std::lock_guard<std::mutex> lock(s_mutex);
    std::uniform_int_distribution<std::size_t> dist(0, sizeof(kAlphabet) - 2);
    std::string id(1, prefix);
    id.reserve(9);
    for (int i = 0; i < 8; ++i) {
        id += kAlphabet[dist(rd)];
    }
    return id;
}

} // namespace agent