/**
 * @file bash_tool.h
 * @brief BashTool — Shell 执行工具
 * @details 执行 shell 命令并返回输出
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include "agent/tool/itool.h"

namespace agent::tool {

/// @brief BashTool — Shell 执行工具
///
/// 执行 shell 命令：
/// - 支持超时设置
/// - 支持工作目录
/// - 返回 stdout/stderr/exit_code
class BashTool : public ITool {
public:
    const std::string& name() const override;
    const std::string& description() const override;
    const std::string& prompt() const override;
    nlohmann::json input_schema() const override;

    ToolResult call(
        const nlohmann::json& input,
        const ToolContext& ctx
    ) override;
};

} // namespace agent::tool
