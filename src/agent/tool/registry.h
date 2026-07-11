#pragma once

/// @brief 工具注册表
///
/// 管理所有可用工具的生命周期：
/// - 注册/注销工具实例
/// - 按名称查找工具
/// - 列举所有工具的 schema（供 LLM function calling 使用）
/// - 对应参考实现中的 tools.ts
