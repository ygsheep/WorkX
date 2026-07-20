/**
 * @file react_loop.h
 * @brief ReActLoop — Reason → Act → Observe 迭代循环
 * @details 实现 Thought/Action/Observation 三阶段显式分离的 agent 循环，
 *          替代 ChatSession 中的扁平 while 循环。
 *          使用原生 function calling（Anthropic/OpenAI），不依赖文本解析。
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <atomic>
#include <chrono>

#include "agent/api/chat_types.h"
#include "agent/api/i_completion_provider.h"
#include "agent/tool/registry.h"
#include "agent/tool/executor.h"
#include "agent/tool/context.h"

namespace agent {

// ============================================================
// ReAct 步骤类型
// ============================================================

/// @brief ReAct 步骤类型
enum class ReActStepType {
    Thought,        ///< LLM 推理（流式输出文本 + 工具调用决策）
    Action,         ///< 工具调用执行
    Observation,    ///< 工具结果回传
    FinalAnswer     ///< 最终回复（终止循环）
};

// ============================================================
// ReActStep — 单个步骤记录
// ============================================================

/// @brief 单个 ReAct 步骤记录
///
/// 每个步骤对应循环中的一个阶段（Thought/Action/Observation/FinalAnswer），
/// 用于 UI 展示、调试和日志记录。
struct ReActStep {
    ReActStepType type = ReActStepType::Thought;  ///< 步骤类型
    int step_number = 0;                          ///< 全局步骤序号（从 1 开始）

    // --- Thought 阶段字段 ---
    std::string thought_text;                     ///< LLM 文本输出
    std::string reasoning;                        ///< LLM 推理内容（thinking）
    std::vector<ToolUse> tool_uses;               ///< LLM 决定调用的工具列表

    // --- Action 阶段字段 ---
    std::string tool_name;                        ///< 当前执行的工具名
    nlohmann::json tool_input;                    ///< 工具输入参数

    // --- Observation 阶段字段 ---
    std::string observation;                      ///< 工具结果文本
    bool is_error = false;                        ///< 工具执行是否出错

    // --- 元信息 ---
    double duration_ms = 0.0;                     ///< 本步骤耗时（毫秒）
};

// ============================================================
// ReActResult — 循环执行结果
// ============================================================

/// @brief ReAct 循环执行结果
///
/// 由 ReActLoop::run() 返回，包含步骤历史、最终输出和统计信息。
/// ChatSession 根据结果状态决定是否重试、发布完成事件等。
struct ReActResult {
    // --- 步骤历史 ---
    std::vector<ReActStep> steps;                 ///< 所有步骤记录

    // --- 最终输出 ---
    std::string final_answer;                     ///< LLM 最终回复文本
    std::string final_reasoning;                  ///< LLM 最终回复的推理内容

    // --- 统计信息 ---
    int total_iterations = 0;                     ///< 总迭代轮数（每轮含 Thought+Action+Observation）
    int total_tool_calls = 0;                     ///< 总工具调用次数
    double total_duration_ms = 0.0;               ///< 总耗时（毫秒）

    // --- token 统计（最后一次 LLM 响应）---
    int32_t prompt_tokens = 0;
    int32_t generated_tokens = 0;
    double prompt_ms = 0.0;
    double generation_ms = 0.0;

    // --- 状态 ---
    bool was_interrupted = false;                 ///< 用户中断
    bool was_error = false;                       ///< 发生错误（流式错误/提交失败/超迭代数）
    std::string error_message;                    ///< 错误信息（was_error=true 时有效）

    // --- 部分输出（中断/错误时可能有部分内容）---
    std::string partial_content;                  ///< 中断时的部分文本
    std::string partial_reasoning;                ///< 中断时的部分推理
};

// ============================================================
// ReActLoop — ReAct 循环本体
// ============================================================

/// @brief ReActLoop — Reason → Act → Observe 迭代循环
///
/// 从 ChatSession 提取的 agent 循环逻辑，实现显式的三阶段分离：
///
/// @par 循环结构
/// ```
/// for (iteration = 1; iteration <= max_iterations; ++iteration) {
///     // === Thought 阶段 ===
///     // 流式读取 LLM 响应，收集 content + reasoning + tool_uses
///     // 通过 on_token 回调实时推送 token 到 UI
///
///     // 终止判断：无 tool_use → FinalAnswer，退出循环
///
///     // === Action 阶段 ===
///     // 对每个 tool_use，通过 ToolExecutor 执行
///     // 通过 on_token 回调推送 [Tool: name] 到 UI
///
///     // === Observation 阶段 ===
///     // 将 tool_result 消息添加到对话历史
///     // 通过 on_token 回调推送 [Result: ...] 到 UI
/// }
/// ```
///
/// @par 终止条件
/// - LLM 无 tool_use（FinalAnswer）
/// - 达到 max_iterations
/// - 用户取消（should_cancel）
/// - 流式错误
///
/// @par 与 ChatSession 的分工
/// - ReActLoop：负责循环逻辑、工具执行、步骤记录
/// - ChatSession：负责会话管理、重试、持久化、状态标志
class ReActLoop {
public:
    // ============================================================
    // 配置与回调类型
    // ============================================================

    /// @brief 循环配置
    struct Config {
        int max_iterations = 25;                  ///< 最大迭代轮数
    };

    /// @brief 步骤回调（每完成一个步骤时调用）
    ///
    /// 用于发布 Agent 事件（AgentStepEvent / ToolCallEvent / ToolResultEvent）。
    /// 在 Thought/Action/Observation/FinalAnswer 每个步骤完成时触发。
    using StepCallback = std::function<void(const ReActStep&)>;

    /// @brief 流式 token 回调（仅 Thought 阶段 LLM 输出 delta 时调用）
    ///
    /// 用于发布 StreamTokenEvent。仅在 LLM 流式输出 content/reasoning delta 时触发。
    /// 工具调用/结果反馈通过 StepCallback（Action/Observation 步骤）发布。
    using TokenCallback = std::function<void(const std::string& content_delta,
                                             const std::string& reasoning_delta)>;

    // ============================================================
    // 构造与析构
    // ============================================================

    /// @brief 构造
    /// @param provider 推理提供者（非拥有，ChatSession 拥有）
    /// @param registry 工具注册表（可为 nullptr，表示无工具）
    /// @param config 循环配置
    ReActLoop(ICompletionProvider* provider,
              std::shared_ptr<tool::ToolRegistry> registry,
              Config config = {});

    ~ReActLoop() = default;

    // 不可拷贝，可移动
    ReActLoop(const ReActLoop&) = delete;
    ReActLoop& operator=(const ReActLoop&) = delete;
    ReActLoop(ReActLoop&&) = default;
    ReActLoop& operator=(ReActLoop&&) = default;

    // ============================================================
    // 主接口
    // ============================================================

    /// @brief 执行 ReAct 循环
    ///
    /// @param messages 对话历史（会被修改：追加 assistant + tool_result 消息）
    /// @param system_prompt 系统提示词
    /// @param tools_schema 工具 schema 数组（注入到 CompletionRequest.tools）
    /// @param should_cancel 外部取消信号
    /// @param on_step 步骤回调（可选，默认 nullptr）
    /// @param on_token 流式 token 回调（可选，默认 nullptr）
    /// @return 循环执行结果
    ReActResult run(
        std::vector<ChatMessage>& messages,
        const std::string& system_prompt,
        const nlohmann::json& tools_schema,
        const std::atomic<bool>& should_cancel,
        StepCallback on_step = nullptr,
        TokenCallback on_token = nullptr
    );

private:
    // ============================================================
    // 内部类型
    // ============================================================

    /// @brief Thought 阶段执行结果
    struct ThoughtResult {
        std::string content;                      ///< LLM 文本输出
        std::string reasoning;                    ///< LLM 推理内容
        std::vector<ToolUse> tool_uses;           ///< LLM 决定调用的工具
        int32_t prompt_tokens = 0;
        int32_t generated_tokens = 0;
        double prompt_ms = 0.0;
        double generation_ms = 0.0;

        /// @brief Thought 状态
        enum Status {
            Completed,                            ///< 流式正常完成
            Error,                                ///< 流式错误
            Cancelled                             ///< 用户取消
        } status = Completed;
    };

    // ============================================================
    // 内部方法
    // ============================================================

    /// @brief 构建 CompletionRequest
    CompletionRequest build_request(
        const std::vector<ChatMessage>& messages,
        const std::string& system_prompt,
        const nlohmann::json& tools_schema
    ) const;

    /// @brief 执行 Thought 阶段（流式读取 LLM 响应）
    ///
    /// 向 provider 提交请求，流式读取响应，收集 content/reasoning/tool_uses。
    /// 每个 content/reasoning delta 通过 on_token 回调推送。
    ///
    /// @param request 推理请求
    /// @param should_cancel 取消信号
    /// @param on_token token 回调
    /// @return Thought 结果
    ThoughtResult execute_thought(
        const CompletionRequest& request,
        const std::atomic<bool>& should_cancel,
        TokenCallback on_token
    );

    // ============================================================
    // 成员
    // ============================================================

    ICompletionProvider* m_provider;              ///< 推理提供者（非拥有）
    std::shared_ptr<tool::ToolRegistry> m_registry; ///< 工具注册表
    std::unique_ptr<tool::ToolExecutor> m_executor; ///< 工具执行器
    Config m_config;                              ///< 循环配置
};

} // namespace agent
