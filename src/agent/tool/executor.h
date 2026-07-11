#pragma once

/// @brief 并发工具执行器
///
/// 负责实际执行 LLM 请求的工具调用：
/// - 解析 LLM 返回的 tool_use 请求
/// - 并发执行多个独立工具调用
/// - 收集结果并构建 tool_result 消息
/// - 支持超时与取消
/// - 对应参考实现中的 StreamingToolExecutor
