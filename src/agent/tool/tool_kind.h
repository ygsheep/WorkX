/**
 * @file tool_kind.h
 * @brief 工具分类枚举（H-3：已迁移至 core/tool_kind.h，本文件为向后兼容 shim）
 * @details 历史位置：ToolType 枚举原寄居于 agent/message/types.h，后迁回 agent/tool/。
 *          H-3 修复分层越界：core/events/events.h 反向依赖 agent/tool/tool_kind.h，
 *          现将枚举迁移至 core/tool_kind.h，agent 反向 include core。
 *
 *          保留本文件以避免破坏现有 #include "agent/tool/tool_kind.h" 调用方。
 * @version 2.0.0
 * @date 2026-07
 */

#pragma once

#include "core/tool_kind.h"
