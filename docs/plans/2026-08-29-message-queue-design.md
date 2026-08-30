# 消息队列设计（Message Queue）

## 目标

模型忙碌时，前端发送的用户消息进入待发送队列；在合适时机批量注入到 ReAct 循环中一起发送，避免消息丢失并提升交互流畅度。

## 双键语义

| 快捷键 | 行为 |
| --- | --- |
| `Enter` | 模型忙碌时仅入队；模型空闲时直接发送（原有行为不变） |
| `Ctrl+Enter` | 入队 + 请求立即冲刷（下个工具轮边界注入） |

## 消息注入点

- 工具轮边界：Action + Observation 阶段结束后、预算递减前，检查 `m_flush_requested`，若为 true 则 `drain_queue()` 合并为单条 user 消息注入。
- 整轮收尾：`run_completion` 结束时若队列仍有未发送消息（无工具轮冲刷机会），收尾冲刷一次。

## 消息合并格式

多条消息合并为单条 user 消息，使用序号 + 分隔线：

```
[排队消息 1/2]
<文本1>
━━━━━━━━━━
[排队消息 2/2]
<文本2>
```

## 队列交互（TUI）

- 输入框上方可折叠条：展示当前队列中待发送消息条数。
- 单条消息可移除（✕ 按钮），命中检测复用 CardHit 机制。
- ViewModel 持有 `MessageQueueState`，通过 `ActionQueueUpdate` 事件驱动更新。

## 事件流

```
TUI composer Ctrl+Enter
  → app.cpp send_input(text, force_flush=true)
  → ChatSession::enqueue_message(text) + request_flush()
  → ChatSession 发布 MessageQueueUpdatedEvent
  → EventBridge → ActionQueueUpdate
  → ViewModel.message_queue 更新 → 卡片重绘
```

## 线程安全

- 队列由 `m_queue_mutex` 保护。
- `drain_queue()` 由 ReActLoop 后台线程调用，持锁拷贝后清空。
- 队列事件发布在锁外执行。

## 文件变更清单

- `src/agent/core/chat_session.h/.cpp`：队列成员 + enqueue/request_flush/remove/drain + 收尾冲刷
- `src/agent/core/react_loop.cpp`：工具轮边界注入
- `src/agent/core/query_engine.cpp`：透传 flush 回调
- `src/agent/core/events/agent_events.h`：MessageQueueUpdatedEvent
- `src/tui/bridge/event_bridge.h/.cpp`：事件转换
- `src/tui/bridge/action.h`：ActionQueueUpdate
- `src/tui/widgets/composer.cpp`：Ctrl+Enter → on_submit_ctrl
- `src/tui/app.cpp`：send_input 分流
- `src/tui/vm/view_model.h/.cpp`：MessageQueueState
- `src/tui/render/markdown_to_elements.*` 或组件渲染：队列卡片
