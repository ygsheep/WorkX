/**
 * @file hook_manager.h
 * @brief 通用 Hook 事件系统 — 调度核心（注册表 + dispatch + 执行器门面）
 * @details Issue #50。HookManager 持有已注册的 HookDefinition 列表（按注册
 *          顺序），dispatch(event, ctx) 时先用一次性编译的 HookMatcher 过滤，
 *          再逐条执行 command/http/prompt/agent 类型的 hook，聚合 blockingError /
 *          preventContinuation / message。进度通过 IEventBus 发布可选。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "agent/hook/hook_event.h"
#include "agent/hook/hook_match.h"

namespace agent {
class ICompletionProvider;
}

namespace agent::hook {

struct HookEntry {
    HookDefinition def;
    HookMatcher matcher;
    bool consumed = false;  // once 语义：已执行过
    uint64_t run_count = 0; // 执行次数（调试/日志）

    explicit HookEntry(HookDefinition d)
        : def(std::move(d)), matcher(def.match) {}
};

/// @brief Hook 生命周期接口。宿主（ReActLoop/ChatSession/AgentTool）持有一个
///        shared_ptr<HookManager> 在各事件点调用 dispatch()。
/// @details 线程安全：注册集中在装配期（启动/会话开始，单线程），dispatch 在
///          ReAct 循环内被调用。dispatch 对 every 执行的 hook 做同步执行，
///          返回聚合结果。async 类型 hook 不阻塞主线程（无同步执行）。
class HookManager {
public:
    /// @brief 注册一个 hook（若同 event+match 已存在则忽略，避免 config+frontmatter 重复）
    void register_hook(HookDefinition def);

    /// @brief 批量注册（清空旧列表）。config 加载 / 会话生命周期切换时调用。
    void register_hooks(std::vector<HookDefinition> defs);

    /// @brief 清除全部 hook（clearSessionHooks 对应）
    void clear() { entries_.clear(); }

    /// @brief 调度事件：过滤匹配的 hook 并顺序执行，聚合结果
    /// @param event 事件类型
    /// @param ctx 事件上下文
    /// @return 聚合结果；无条件命中时返回默认（无效）
    HookResult dispatch(HookEvent event, const HookContext& ctx);

    /// @brief 是否注册了任何 hook（空 → 调用方可零开销短路）
    bool empty() const noexcept { return entries_.empty(); }

    /// @brief 已注册 hook 数（调试/UI）
    size_t size() const noexcept { return entries_.size(); }

    /// @brief 注入 LLM provider（prompt/agent 类型需要；非拥有指针）
    /// @note 必须在 dispatch 前设置；为 nullptr 时 prompt/agent hook 降级为未就绪消息
    void set_provider(agent::ICompletionProvider* provider) noexcept { provider_ = provider; }

private:
    std::vector<HookEntry> entries_;
    agent::ICompletionProvider* provider_ = nullptr;

    /// @brief 执行单条 hook，返回其 HookResult
    HookResult run_hook(const HookEntry& entry, const HookContext& ctx);

    /// @brief 执行附带的输出/结果聚合逻辑（message 等）
    void aggregate(const HookResult& r, HookResult& out) const noexcept;

    // 四种类型执行器
    HookResult run_command(const HookDefinition& def, const HookContext& ctx);
    HookResult run_http(const HookDefinition& def, const HookContext& ctx);
    HookResult run_prompt(const HookDefinition& def, const HookContext& ctx);
    HookResult run_agent(const HookDefinition& def, const HookContext& ctx);
};

} // namespace agent::hook