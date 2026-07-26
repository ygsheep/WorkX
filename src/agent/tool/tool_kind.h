/**
 * @file tool_kind.h
 * @brief 工具分类枚举
 * @details 工具类型分类，用于 ToolCallEvent.tool_type 字段，供 UI 渲染层
 *          按类别展示工具调用。属于 tool 领域的元数据，定义在 agent::tool
 *          命名空间下。
 *
 *          历史位置：原寄居于 agent/message/types.h 的 agent:: 命名空间，
 *          属于反向依赖（tool 领域类型错位到 message 层）。本次迁回 tool/。
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

namespace agent::tool {

/// @brief 工具类型分类（用于 UI 渲染分组）
enum class ToolType {
    ReadFile,       ///< 文件读取
    WriteFile,      ///< 文件写入
    EditFile,       ///< 文件编辑
    Execute,        ///< Shell 命令
    Search,         ///< 搜索（grep/find）
    Agent,          ///< 子代理
    Other           ///< 其他/未知
};

} // namespace agent::tool
