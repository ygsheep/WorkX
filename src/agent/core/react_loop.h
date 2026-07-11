#pragma once

/// @brief ReAct 循环本体
///
/// 实现 Reason → Act → Observe 的迭代循环：
/// - 向 LLM 发送当前上下文，获取下一步动作
/// - 执行工具调用（Act）
/// - 将工具结果注入上下文（Observe）
/// - 判断终止条件（最终回复、最大轮次、用户中断）
/// - 对应参考实现中的 query.ts 核心循环逻辑
