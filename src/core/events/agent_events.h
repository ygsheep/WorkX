/**
 * @file agent_events.h
 * @brief Agent 编排事件类型（H-10：从 events.h 按域拆分）
 * @details Agent 推理步骤、工具调用、工具结果、Agent 编排完成等事件。
 *          订阅方按需 include 本文件，避免引入系统/流式事件。
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <vector>

#include "core/tool_kind.h"  // C-3：直接引用 core 层规范位置，避免 core→agent 分层越界

namespace agent {

// ============================================================
// Agent 事件（Agent → TUI，未来）
// ============================================================

/// @brief Agent 推理步骤
struct AgentStepEvent {
    std::string step_id;
    int32_t step_number = 0;
    std::string description;
};

/// @brief Agent 调用工具
struct ToolCallEvent {
    std::string tool_name;
    std::string arguments;
    std::string call_id;
    agent::tool::ToolType tool_type = agent::tool::ToolType::Other;
};

/// @brief 工具返回结果
struct ToolResultEvent {
    std::string call_id;
    std::string result;
    bool is_error = false;
};

/// @brief Agent 编排完成
struct AgentDoneEvent {
    std::string final_response;
    int32_t total_steps = 0;
    int32_t total_tool_calls = 0;
    double total_duration_ms = 0.0;
};

/// @brief 缓存诊断事件（DeepSeek 硬盘缓存命中率劣化归因）
/// @details 当某轮缓存命中率显著下降时发布，用于 UI 显示归因。
///          reasons 取值："system" / "tools" / "log_rewrite"
struct CacheDiagnosticsEvent {
    std::string session_id;
    std::string prefix_hash;             ///< 当前前缀形状 hash（system + tools 联合）
    bool prefix_changed = false;         ///< 与上一轮相比前缀是否变化
    std::vector<std::string> reasons;    ///< 变化原因（空表示无变化）
    int32_t cache_hit_tokens = 0;        ///< 本轮命中 token 数
    int32_t cache_miss_tokens = 0;       ///< 本轮未命中 token 数
};

/// @brief 压缩暂停事件（DS_CACHE H-3：卡死守卫触发/恢复）
/// @details 当 CacheAwareCompactor 连续 compact 达阈值时发布（paused=true），
///          当 ratio 回落到 soft 以下自愈恢复时发布（paused=false）。
///          UI 据此提示"压缩已暂停，前缀重新 append-only 增长以恢复缓存命中"。
struct CompactionPausedEvent {
    std::string session_id;
    bool paused = true;                  ///< true=卡死守卫触发暂停；false=自愈恢复
    int32_t consecutive_compacts = 0;    ///< 触发时的连续 compact 次数
    int32_t tokens_before = 0;           ///< 触发时的 token 数
    float ratio = 0.0f;                  ///< 触发时的窗口占用比
    std::string notice;                  ///< 人类可读说明
};

} // namespace agent
