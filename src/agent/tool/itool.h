#pragma once

/// @brief ITool 接口 — 工具抽象基类
///
/// 所有 Agent 可调用工具的统一接口：
/// - 名称、描述、参数 schema 声明
/// - execute() 执行工具逻辑并返回结果
/// - 对应参考实现中的 Tool.ts（精简版）
