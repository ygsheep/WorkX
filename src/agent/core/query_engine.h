#pragma once

/// @brief QueryEngine — Agent 会话生命周期管理
///
/// 负责编排一次完整的 Agent 查询流程：
/// - 接收用户输入，构建请求上下文
/// - 协调 ReAct 循环、工具调用、权限检查
/// - 管理 streaming 输出与会话状态流转
