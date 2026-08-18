/**
 * @file action.h
 * @brief TuiAction — 后台事件回调 → UI 线程的不可变动作
 * @details EventBridge 订阅 EventBus 后只入队轻量的 action，不持锁碰 UI 状态；
 *          UI 线程每帧 drain 后应用到 ViewModel。见
 *          docs/plans/2026-08-17-ftxui-tui-design.md §3。
 * @version 0.1.0（实验）
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>
#include "agent/api/i_completion_provider.h"
#include "agent/model/provider_config.h"
#include "core/events/agent_events.h"  // agent::AskUserResult

namespace ftxtui {

/// @brief 追加一条已完成的消息（用户/历史回放/本地命令回显）
struct ActionAppendMessage {
    std::string role;   ///< "user" / "assistant"
    std::string text;
};

/// @brief 流式正文增量（追加到当前流式消息节点）
struct ActionTokenDelta {
    std::string content_delta;
};

/// @brief 流式思考增量（追加到当前流式消息的 reasoning 缓冲）
struct ActionReasoningDelta {
    std::string delta;
};

/// @brief ReAct 单步 LLM 输出结束（仍有工具待执行，轻量收尾）
struct ActionStepDone {};

/// @brief 整轮结束（提交当前流式消息，token 统计，状态回 IDLE）
struct ActionTurnDone {
    std::string full_content;
    std::string full_reasoning;
    bool was_interrupted = false;
    bool is_local_command = false;
    int32_t prompt_tokens = 0;
    int32_t generated_tokens = 0;
    int32_t cache_read_input_tokens = 0;
    double prompt_ms = 0.0;
    double generation_ms = 0.0;
};

/// @brief 推理错误
struct ActionError {
    std::string message;
};

/// @brief 工具调用开始（创建可折叠工具块）
struct ActionBeginTool {
    std::string tool_name;
    std::string call_id;
    std::string arguments;  ///< JSON 字符串
};

/// @brief 工具结果回传（写入工具块，可展开）
struct ActionEndTool {
    std::string call_id;
    std::string result;
    bool is_error = false;
};

/// @brief Agent 编排完成（汇总工具次数/耗时/最终答复）
/// @details 携带 final_answer，若当前没有流式消息（例如纯工具调用后直接结束），
///          用它兜底创建一条 assistant 消息。
struct ActionAgentDone {
    std::string final_answer;
    int32_t total_steps = 0;
    int32_t total_tool_calls = 0;
    double total_duration_ms = 0.0;
};

/// @brief 忙碌状态（后台是否有生成任务）
struct ActionSetBusy {
    bool busy = false;
};

/// @brief 权限模式标签更新
struct ActionPermissions {
    std::string label;  ///< "" / "plan" / "bypass"
};

/// @brief AskUser 模态请求（携带回填 promise）
struct ActionAskUser {
    nlohmann::json questions;
    int32_t timeout_ms = 0;
    std::shared_ptr<std::promise<agent::AskUserResult>> result_promise;
    std::shared_ptr<std::atomic<bool>> cancel_flag;
};

/// @brief AskUser 超时（关闭模态，返回 cancelled）
struct ActionAskUserTimeout {};

/// @brief 缓存诊断事件（DeepSeek 缓存命中率劣化归因）
/// @details prefix_changed=true 时提示缓存前缀变化原因，辅助理解命中率下降。
struct ActionCacheDiagnostics {
    std::string prefix_hash;
    bool prefix_changed = false;
    std::vector<std::string> reasons;   ///< 变化原因（"system"/"tools"/"log_rewrite"）
    int32_t cache_hit_tokens = 0;
    int32_t cache_miss_tokens = 0;
};

/// @brief 压缩暂停/恢复事件（DS_CACHE H-3 卡死守卫）
struct ActionCompactionPaused {
    bool paused = true;                 ///< true=守卫触发暂停；false=自愈恢复
    int32_t consecutive_compacts = 0;
    std::string notice;                 ///< 人类可读说明
};

/// @brief 子 Agent 进度增量（AgentTool → 订阅者）
struct ActionSubAgentProgress {
    std::string task_id;
    int32_t step_number = 0;
    std::string step_type;              ///< "thought"/"action"/"observation"/"final"
    std::string content;
};

/// @brief 子 Agent 完成（AgentTool → 订阅者）
struct ActionSubAgentCompleted {
    std::string task_id;
    std::string final_answer;
    bool was_error = false;
    double duration_ms = 0.0;
};

/// @brief 请求 UI 关闭（/exit）
struct ActionShutdown {};

/// @brief 临时提示（toast / 命令回显）
struct ActionToast {
    std::string text;
    bool is_error = false;
};

/// @brief 模型列表加载完成（App 后台线程 list_models 后入队）
struct ActionModelsLoaded {
    std::vector<std::string> models;
};

/// @brief 会话列表条目（UI 侧轻量拷贝，避免 action.h 依赖 SessionStore）
struct SessionLite {
    std::string title;       ///< 会话标题（无则回退 session_id）
    std::string file_path;   ///< JSONL 文件路径（恢复用）
    int message_count = 0;
};

/// @brief 会话列表加载完成（App 后台线程 list_sessions 后入队）
struct ActionSessionsLoaded {
    std::vector<SessionLite> sessions;
};

/// @brief 供应商切换完成（后台 create_backend 成功后入队；provider 移交 UI 线程）
/// @details UI 线程处理时执行 set_provider + import_messages（保留对话继续），
///          成功后再写配置（apply_provider_switch）。
struct ActionProviderSwitched {
    std::unique_ptr<agent::ICompletionProvider> provider;  ///< 新后端（已 initialize）
    std::string model_name;                                ///< 新模型名（可能为空）
    agent::ProviderConfigEntry entry;                      ///< 已选条目（写配置用）
};

/// @brief 供应商切换失败（后台 create_backend 创建/初始化失败）
struct ActionProviderSwitchFailed {
    std::string provider_name;  ///< 显示名（提示用）
};

/// @brief 统一动作类型
using Action = std::variant<
    ActionAppendMessage,
    ActionTokenDelta,
    ActionReasoningDelta,
    ActionStepDone,
    ActionTurnDone,
    ActionError,
    ActionBeginTool,
    ActionEndTool,
    ActionAgentDone,
    ActionSetBusy,
    ActionPermissions,
    ActionAskUser,
    ActionAskUserTimeout,
    ActionCacheDiagnostics,
    ActionCompactionPaused,
    ActionSubAgentProgress,
    ActionSubAgentCompleted,
    ActionShutdown,
    ActionToast,
    ActionModelsLoaded,
    ActionSessionsLoaded,
    ActionProviderSwitched,
    ActionProviderSwitchFailed
>;

}  // namespace ftxtui