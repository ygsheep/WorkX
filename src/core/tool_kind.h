/**
 * @file tool_kind.h
 * @brief 工具分类枚举（H-3 从 agent/tool/tool_kind.h 迁移至 core 层）
 * @details 原位于 agent/tool/tool_kind.h，因 core/events/events.h 的 ToolCallEvent
 *          使用 agent::tool::ToolType 导致 core 反向依赖 agent。现将枚举迁移至
 *          core/tool_kind.h，agent 反向 include core，消除分层越界。
 *
 *          保留原 agent::tool 命名空间以避免破坏现有调用方。
 * @version 2.0.0
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
