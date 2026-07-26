/**
 * @file types.h
 * @brief 跨层通信事件类型定义（已迁移至 core/events/events.h）
 * @details 本文件仅作为向后兼容 shim，实际定义位于 core/events/events.h。
 *
 *          迁移原因：这些事件类型由 TUI / Agent / Core 各层共享，属于
 *          核心基础设施，不应依赖 agent/message/ 业务层。原定义已迁至
 *          core/events/events.h，本文件保留以避免破坏现有 #include。
 *
 *          注：ChatMessage / ToolUse / CompletionRequest / StreamChunk 等
 *          消息与 DTO 类型位于 agent/api/chat_types.h；ToolType 枚举位于
 *          agent/tool/tool_kind.h。本文件不再含任何类型定义。
 * @version 2.0.0
 * @date 2026-07
 */

#pragma once

#include "core/events/events.h"
