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
#include <future>
#include <memory>
#include <atomic>
#include <nlohmann/json.hpp>

#include "core/tool_kind.h"  // C-3：直接引用 core 层规范位置，避免 core→agent 分层越界

namespace agent {

// ============================================================
// AskUser 宿主交互结果（宿主无关，TUI 等宿主自行转换自有类型）
// ============================================================

/// @brief AskUser 提问结果（宿主无关类型）
/// @details 由 TUI ChoicePanel 等宿主在 set_value 时填回，
///          与 tui::ChoiceResult 同构，避免 core/agent 依赖 UI 层。
struct AskUserResult {
    bool submitted = false;      ///< true=用户提交, false=取消/超时
    /// @brief 答案映射: question → answer（单选/多选/自定义输入）
    std::vector<std::pair<std::string, std::string>> answers;
};

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

/// @brief AskUser 请求事件（AskUserTool → TUI，触发 ChoicePanel 模态）
/// @details AskUserTool 在工作线程发布本事件后阻塞等待 result_promise；
///          TUI 主循环 drain 异步事件后弹出 ChoicePanel，用户操作完成时
///          调用 result_promise->set_value() 回填结果唤醒工作线程。
///          timeout_ms > 0 时，工作线程等待超时后自动返回 timeout 状态，
///          同时置位 cancel_flag 并发布 AskUserTimeoutEvent 通知 TUI 关闭面板。
struct AskUserRequestEvent {
    std::string session_id;
    nlohmann::json questions;   ///< 原样转交 parse_choice_config 的 JSON
    int32_t timeout_ms = 0;     ///< 超时毫秒（0=不限时；>0 超时自动返回）
    /// @brief 结果回填通道：TUI 设置 value 后唤醒阻塞的 AskUserTool
    /// @details shared_ptr 使事件按值传递时 promise 仍共享同一实例
    std::shared_ptr<std::promise<AskUserResult>> result_promise;
    /// @brief 取消标志：工作线程超时后置位，TUI 主循环检查后关闭 ChoicePanel
    /// @details shared_ptr<atomic<bool>> 使事件按值传递时标志仍共享同一实例。
    ///          nullptr 表示不支持取消（timeout_ms == 0 时可为空）。
    std::shared_ptr<std::atomic<bool>> cancel_flag;
};

/// @brief AskUser 超时事件（AskUserTool → TUI，通知主循环关闭 ChoicePanel）
/// @details AskUserTool 在工作线程超时后发布本事件。ChatRenderer 订阅后
///          调用 Terminal::wake_main_loop()，使阻塞在 read_char 的主循环
///          返回 KEY_WAKE，run_choice_panel 检查 cancel_flag 后返回 cancelled。
struct AskUserTimeoutEvent {
    std::string session_id;
};

/// @brief 进入计划模式事件（#28：EnterPlanModeTool → TUI/宿主）
/// @details AI 主动请求进入计划模式（只读调研阶段）。宿主收到后可展示
///          "计划模式"状态；权限模式切换由 ToolContext 回调完成，
///          本事件仅用于 UI 通知。
struct EnterPlanModeEvent {
    std::string session_id;
    std::string reason;   ///< 进入计划模式的原因（AI 说明，可空）
};

/// @brief 退出计划模式事件（#28：ExitPlanModeV2Tool → TUI/宿主）
/// @details AI 完成规划后发布：携带方案文本与用户批准结果。
///          approved=true 表示用户批准方案，可进入执行阶段。
struct ExitPlanModeEvent {
    std::string session_id;
    std::string plan;       ///< 方案文本（改哪些文件、风险点等）
    bool approved = false;  ///< 用户是否批准
};

/// @brief 子 Agent 后台任务完成事件（AgentTool → 订阅者，v1.1.0 后台结果自动回送）
/// @details 后台子 Agent 结束后由 AgentTool 发布，携带 task_id 与最终结果摘要，
///          使父会话/UI 等订阅者无需轮询 TaskOutput 即可感知子任务完成。
///          ⚠️ 仅作通知，不注入父 LLM 上下文（避免长输出刷屏父会话，见设计决策）。
///          完整输出仍通过 TaskOutputTool 按 task_id 读取。
struct SubAgentCompletedEvent {
    std::string task_id;        ///< 子 Agent 任务 id（AgentTool 生成的 'a'+8 随机）
    std::string final_answer;   ///< 最终答案（Final: ...）或错误信息（Error: ...）
    bool was_error = false;     ///< 子任务是否以错误结束
    float duration_ms = 0.0f;   ///< 子 Agent 循环耗时（毫秒）
};

} // namespace agent
