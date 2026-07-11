#pragma once

/// @brief 工具执行上下文
///
/// 在工具执行过程中传递的运行时信息：
/// - 当前会话 ID 与请求 ID
/// - 工作目录路径
/// - 权限模式
/// - 中断信号（CancellationToken）
/// - 对应参考实现中的 ToolUseContext（精简版）
