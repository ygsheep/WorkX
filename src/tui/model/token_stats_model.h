/**
 * @file token_stats_model.h
 * @brief Token 统计模型（P2 从 ChatRenderer 提取）
 * @details 维护会话级 token 统计：消息计数、累计 token、cache 命中。
 *          封装用户输入估算与 StreamDoneEvent 用量更新逻辑，
 *          使 ChatRenderer 不再直接管理统计状态。
 *
 *          线程安全：atomic 字段，可在事件回调线程访问。
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "agent/api/chat_types.h"
#include "agent/compact/token_count.h"

namespace tui {

/**
 * @brief Token 统计模型
 * @details 从 ChatRenderer 提取的纯数据模型，负责 token 计数与 cache 命中统计。
 *          调用方（ChatRenderer）负责在更新后同步到 StatusBar。
 */
class TokenStatsModel {
public:
    /// @brief 累加用户输入估算 token（provider 不返回 usage 时使用）
    /// @param text 用户输入文本
    void add_user_input(const std::string& text) {
        if (text.empty()) return;
        agent::ChatMessage tmp = agent::ChatMessage::user(text);
        m_total_tokens.fetch_add(agent::compact::estimate_messages_tokens({tmp}));
    }

    /// @brief 从 StreamDoneEvent 用量更新统计（provider 返回 usage 时使用）
    /// @details Anthropic 命中 prompt cache 时 prompt_tokens 不含 cache 部分，需单独累加。
    ///          generated_tokens 为本次生成量，直接累加到总量。
    ///          DeepSeek 硬盘缓存命中累计到会话级统计（hit_total/miss_total）。
    /// @param prompt_tokens prompt token 数
    /// @param generated_tokens 生成 token 数
    /// @param cache_creation_input_tokens cache 创建 token 数
    /// @param cache_read_input_tokens cache 命中 token 数
    /// @param prompt_cache_hit_tokens DeepSeek 硬盘缓存命中 token 数
    /// @param prompt_cache_miss_tokens DeepSeek 硬盘缓存未命中 token 数
    void update_from_usage(int32_t prompt_tokens,
                           int32_t generated_tokens,
                           int32_t cache_creation_input_tokens,
                           int32_t cache_read_input_tokens,
                           int32_t prompt_cache_hit_tokens = 0,
                           int32_t prompt_cache_miss_tokens = 0) {
        m_total_tokens.store(prompt_tokens
                             + cache_creation_input_tokens
                             + cache_read_input_tokens
                             + generated_tokens);
        m_cache_read_tokens.store(cache_read_input_tokens);
        // DeepSeek 会话级累计（原子累加，用于计算平均命中率）
        if (prompt_cache_hit_tokens > 0 || prompt_cache_miss_tokens > 0) {
            m_ds_cache_hit_total.fetch_add(prompt_cache_hit_tokens);
            m_ds_cache_miss_total.fetch_add(prompt_cache_miss_tokens);
        }
    }

    /// @brief 估算响应内容并累加（provider 不返回 usage 时使用）
    /// @param full_content 完整正文
    /// @param full_reasoning 完整推理内容
    void add_response_estimate(const std::string& full_content,
                               const std::string& full_reasoning) {
        agent::ChatMessage tmp = agent::ChatMessage::assistant(full_content);
        tmp.reasoning_content = full_reasoning;
        m_total_tokens.fetch_add(agent::compact::estimate_message_tokens(tmp));
        // provider 未返回 usage，清除 cache 显示
        m_cache_read_tokens.store(0);
    }

    /// @brief 消息计数 +1
    void increment_message_count() { m_message_count.fetch_add(1); }

    /// @brief 重置所有统计（新会话）
    void reset() {
        m_message_count.store(0);
        m_total_tokens.store(0);
        m_cache_read_tokens.store(0);
        m_ds_cache_hit_total.store(0);
        m_ds_cache_miss_total.store(0);
    }

    // === Getters ===
    int32_t message_count() const { return m_message_count.load(); }
    int32_t total_tokens() const { return m_total_tokens.load(); }
    int32_t cache_read_tokens() const { return m_cache_read_tokens.load(); }

    /// @brief DeepSeek 会话级缓存命中率（0-100，无数据时返回 -1）
    int32_t ds_cache_hit_rate() const {
        int64_t hit = m_ds_cache_hit_total.load();
        int64_t miss = m_ds_cache_miss_total.load();
        int64_t total = hit + miss;
        if (total == 0) return -1;
        return static_cast<int32_t>(hit * 100 / total);
    }

private:
    std::atomic<int32_t> m_message_count{0};
    std::atomic<int32_t> m_total_tokens{0};
    std::atomic<int32_t> m_cache_read_tokens{0};
    std::atomic<int64_t> m_ds_cache_hit_total{0};   ///< DeepSeek 会话级缓存命中累计
    std::atomic<int64_t> m_ds_cache_miss_total{0};  ///< DeepSeek 会话级缓存未命中累计
};

} // namespace tui
