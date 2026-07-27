/**
 * @file web_fetch_tool.h
 * @brief WebFetchTool — 网页抓取工具
 * @details 抓取 URL 内容并转为 Markdown
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include "agent/tool/itool.h"

namespace agent::tool {

/// @brief WebFetchTool — 网页抓取工具
///
/// 抓取指定 URL 的网页内容：
/// - 将 HTML 转为 Markdown
/// - 支持指定 prompt 提取关键信息
class WebFetchTool : public ITool {
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
