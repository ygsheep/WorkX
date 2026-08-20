/**
 * @file agent_tool.h
 * @brief AgentTool — 子 Agent 调度工具
 * @details 启动子 Agent 执行复杂任务
 * @version 1.3.1
 * @date 2026-07
 */

#pragma once

#include <string>
#include "agent/tool/itool.h"

namespace agent::tool {

/// @brief AgentTool — 子 Agent 调度工具
///
/// 启动子 Agent 处理独立子任务：
/// - 支持指定 prompt 和工具集
/// - 子 Agent 独立运行并返回结果
class AgentTool : public ITool {
public:
    const std::string& name() const override;
    const std::string& description() const override;
    const std::string& prompt() const override;
    nlohmann::json input_schema() const override;

    ResultV2<ToolResult> call(
        const nlohmann::json& input,
        const ToolContext& ctx
    ) const override;
};

} // namespace agent::tool
