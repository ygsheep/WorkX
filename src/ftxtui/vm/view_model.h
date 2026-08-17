/**
 * @file view_model.h
 * @brief ViewModel — 文档模型（UI 线程独有），Apply 后台 action
 * @details 所有 UI 状态归 UI 线程读写；后台只通过 ActionQueue 推送 action。
 *          本文件只做数据变更，渲染在 app.cpp 单独进行。
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include "bridge/action.h"
#include "vm/message_node.h"

namespace ftxtui {

/// @brief 侧栏模型
struct SidebarModel {
    std::string title;          ///< 会话标题
    std::string model;          ///< 模型名
    std::string project;        ///< 项目名
    std::string branch;         ///< git 分支
    std::string agent;          ///< agent 名
    int context_limit = 0;      ///< 上下文上限（token）
    int context_used = 0;       ///< 已用（token）
    double cost_usd = 0.0;      ///< 会话成本
    std::string permission;     ///< 权限模式标签 ""/"plan"/"bypass"
    bool visible = true;        ///< 侧栏是否可见（窄屏折叠）
};

/// @brief 折叠卡片默认配置（新卡片创建时的展开/收起初始值）
struct CardDefaults {
    bool reasoning_expanded = true;  ///< 思考卡默认展开
    bool tool_expanded = false;      ///< 工具卡默认收起
};

/// @brief 顶层视图模型
class ViewModel {
public:
    std::vector<MessageNode> messages;
    SidebarModel sidebar;
    bool busy = false;          ///< 是否正在生成/忙碌
    std::string prompt_echo;    ///< 待显示的命令回显/提示
    bool pending_exit = false;  ///< 收到 /exit，UI 应退出
    CardDefaults card_defaults; ///< 折叠卡片默认配置

    /// @brief 应用一个动作
    /// @return 状态是否有变化（用于决定是否重绘）
    bool apply(const Action& action);

    /// @brief 获取当前正在流式的消息（不存在则创建一条 assistant）
    MessageNode& active_stream();
    bool has_active_stream() const;

    /// @brief 会话内累计 token（本实现用最后一次 TurnDone 的统计近似）
    int32_t total_tokens = 0;

private:
    // 单 action 分派（由 apply 的 std::visit 调用）
    bool apply_variant(const ActionAppendMessage&);
    bool apply_variant(const ActionTokenDelta&);
    bool apply_variant(const ActionReasoningDelta&);
    bool apply_variant(const ActionStepDone&);
    bool apply_variant(const ActionTurnDone&);
    bool apply_variant(const ActionError&);
    bool apply_variant(const ActionBeginTool&);
    bool apply_variant(const ActionEndTool&);
    bool apply_variant(const ActionAgentDone&);
    bool apply_variant(const ActionSetBusy&);
    bool apply_variant(const ActionPermissions&);
    bool apply_variant(const ActionAskUser&);
    bool apply_variant(const ActionAskUserTimeout&);
    bool apply_variant(const ActionShutdown&);
    bool apply_variant(const ActionToast&);
    bool apply_variant(const ActionModelsLoaded&);
};

}  // namespace ftxtui