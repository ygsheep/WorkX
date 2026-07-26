# WorkX 日志埋点规范

> **状态**: Phase 0 产出
> **关联**: [ARCH_REFACTOR_PLAN.md](./ARCH_REFACTOR_PLAN.md) Phase 0 / G-1
> **目的**: 为架构重构提供可观测性基础，统一各模块日志埋点约定

---

## 一、日志系统现状

### 1.1 已有能力

`lib/liblogger/logger.h` 提供完整 API：

| 宏 | 级别 | 用途 |
|----|------|------|
| `LOG_TRACE(fmt, ...)` | TRACE | 极细粒度跟踪（默认不输出） |
| `LOG_DEBUG(fmt, ...)` | DEBUG | 调试信息 |
| `LOG_INFO(fmt, ...)` | INFO | 一般信息 |
| `LOG_WARN(fmt, ...)` | WARN | 警告 |
| `LOG_ERROR(fmt, ...)` | ERROR | 错误 |
| `LOG_FATAL(fmt, ...)` | FATAL | 致命错误 |

特性：
- 线程安全（atomic + mutex）
- 异步文件写入（独立 writer 线程）
- 重复过滤（1s 内相同消息去重）
- 支持 `std::format` 格式化

### 1.2 当前问题

- 全项目仅 3 处使用 LOG（`sse_parser.cpp` / `remote_backend.cpp`）
- 核心模块（EventBus / TaskManager / ReActLoop / ChatSession / ToolExecutor / HttpClient）零日志
- 无统一上下文字段（session_id / task_id 缺失）

---

## 二、级别使用约定

### 2.1 级别选择原则

| 级别 | 使用场景 | 示例 |
|------|---------|------|
| **TRACE** | 单次函数调用入口/出口、变量值、循环迭代 | `LOG_TRACE("push token to buffer, len={}", delta.size())` |
| **DEBUG** | 关键决策点、状态变更、外部调用入参出参 | `LOG_DEBUG("ReActLoop thought {} tool_uses={}", step, n)` |
| **INFO** | 生命周期事件（启动/停止）、用户可见行为 | `LOG_INFO("ChatSession started, model={}", model_name)` |
| **WARN** | 可恢复的异常、降级、重试、配置回退 | `LOG_WARN("provider list_models failed, fallback to capability table")` |
| **ERROR** | 不可恢复错误、异常捕获、操作失败 | `LOG_ERROR("tool '{}' threw: {}", tool_name, e.what())` |
| **FATAL** | 进程级致命错误，无法继续 | `LOG_FATAL("EventBus singleton double-init")` |

### 2.2 反模式（禁止）

- ❌ 在热循环里用 INFO（高频日志拖垮性能）
- ❌ 用 `std::cout` 替代 LOG（绕过日志系统）
- ❌ LOG 消息只输出英文且无上下文（如 `LOG_ERROR("failed")`）
- ❌ 在析构函数里 LOG（可能触发 Logger 自身析构竞争）

---

## 三、上下文字段规范

### 3.1 必须携带的字段

每个模块的日志消息应包含**上下文标识符**，便于跨模块追踪同一会话/任务：

| 模块 | 必带字段 | 示例格式 |
|------|---------|---------|
| **ChatSession** | `session_id` | `[sess=abc123] send_message: ...` |
| **ReActLoop** | `session_id` + `step` | `[sess=abc123 step=3] thought: 2 tool_uses` |
| **TaskManager** | `task_id` + `task_name` | `[task=42 name=completion] started` |
| **EventBus** | `event_type` + 队列大小 | `[event=StreamTokenEvent queue=128] publish_async` |
| **ToolExecutor** | `tool_name` + `tool_use_id` | `[tool=Bash id=tu_01] execute` |
| **HttpClient** | `url` + `method` | `[POST https://api.anthropic.com/v1/messages] start` |
| **ContextCompressor** | `session_id` + 压缩前后 token 数 | `[sess=abc123] compress 12000→8000 tokens` |

### 3.2 格式约定

```
[模块标识符] 简短描述 + 关键键值对
```

**示例**：
```cpp
LOG_DEBUG("[sess={} step={}] thought: {} tool_uses, model={}",
          session_id, step_num, thought.tool_uses.size(), model_name);

LOG_WARN("[sess={}] provider list_models failed: {}, fallback to capability table",
         session_id, e.what());

LOG_ERROR("[tool={} id={}] threw exception: {}",
          tu.name, tu.id, e.what());
```

---

## 四、各模块埋点清单

### 4.1 EventBus（Phase 1）

| 位置 | 级别 | 消息 |
|------|------|------|
| `subscribe<T>` | DEBUG | `[event={}] subscriber added, total={}` |
| `unsubscribe<T>` | DEBUG | `[event={}] subscriber removed, total={}` |
| `publish<T>` | TRACE | `[event={}] publish sync to {} subscribers` |
| `publish_async<T>` | DEBUG | `[event={}] enqueue, queue_size={}` |
| `process_async_events` | DEBUG | `process {} async events, remaining={}` |
| 队列积压 > 100 | WARN | `async queue backlog: {} events` |
| `clear()` | INFO | `EventBus cleared: {} subscribers, {} async events dropped` |

### 4.2 TaskManager（Phase 1/2）

| 位置 | 级别 | 消息 |
|------|------|------|
| `start(task)` | INFO | `[task={} name={}] started` |
| `update()` 清理 | DEBUG | `cleanup {} finished tasks` |
| `cancel(task_id)` | INFO | `[task={} name={}] cancel requested` |
| Task::execute 入口 | DEBUG | `[task={} name={}] execute begin` |
| Task::execute 出口 | DEBUG | `[task={} name={}] execute end, status={}, duration={}ms` |
| Task 异常捕获 | ERROR | `[task={} name={}] unhandled exception: {}` |
| ThreadPool 入队 | TRACE | `[pool] enqueue, pending={}` |
| ThreadPool 出队 | TRACE | `[pool] dequeue, active={}` |

### 4.3 ReActLoop（Phase 3.5）

| 位置 | 级别 | 消息 |
|------|------|------|
| Thought 阶段开始 | DEBUG | `[sess={} step={}] thought begin, model={}` |
| Thought 完成 | DEBUG | `[sess={} step={}] thought end: {} tool_uses, prompt_tokens={}, generated_tokens={}, duration={}ms` |
| Action 执行 | INFO | `[sess={} step={}] action: tool={}, input={}` |
| Observation 完成 | DEBUG | `[sess={} step={}] observation: {} bytes, is_error={}` |
| 工具并行执行 | INFO | `[sess={} step={}] parallel {} tools: [{}]` |
| max_iterations 达到 | WARN | `[sess={}] reached max_iterations={}, stopping` |
| should_cancel 触发 | INFO | `[sess={}] cancellation requested at step {}` |
| 重试 | WARN | `[sess={}] retry attempt {}/{} due to: {}` |

### 4.4 ChatSession（Phase 3.5）

| 位置 | 级别 | 消息 |
|------|------|------|
| `send_message` 入口 | INFO | `[sess={}] user message: {} chars` |
| `run_completion` 入口 | DEBUG | `[sess={}] run_completion, retry={}` |
| 流式开始 | DEBUG | `[sess={}] stream started` |
| 流式完成 | INFO | `[sess={}] stream done: prompt={}+cache_read={}+generated={} tokens, duration={}ms` |
| 流式错误 | ERROR | `[sess={}] stream error: {}` |
| 重试触发 | WARN | `[sess={}] retry {}/{}` |
| 会话保存 | DEBUG | `[sess={}] saved to {}` |
| 会话加载 | DEBUG | `[sess={}] loaded from {}` |

### 4.5 ToolExecutor（Phase 3）

| 位置 | 级别 | 消息 |
|------|------|------|
| `execute` 入口 | DEBUG | `[tool={} id={}] execute, input_size={} bytes` |
| 权限检查拒绝 | WARN | `[tool={} id={}] permission denied: {}` |
| 工具未找到 | ERROR | `[tool={}] not registered` |
| 工具异常 | ERROR | `[tool={} id={}] threw: {}` |
| 结果截断 | WARN | `[tool={} id={}] result truncated: {} → {} bytes` |
| `execute` 出口 | DEBUG | `[tool={} id={}] done, result_size={} bytes, is_error={}` |

### 4.6 HttpClient（Phase 3.5/6）

| 位置 | 级别 | 消息 |
|------|------|------|
| 请求开始 | INFO | `[{} {}] start` |
| 请求成功 | DEBUG | `[{} {}] success: {} bytes, {}ms` |
| 请求失败 | ERROR | `[{} {}] failed: {}` |
| 超时 | WARN | `[{} {}] low_speed timeout after {}s` |
| 重试 | WARN | `[{} {}] retry {}/{} after {}ms` |
| SSE 解析错误 | WARN | `[sse] parse error at offset {}: {}` |

### 4.7 ConfigManager（Phase 4）

| 位置 | 级别 | 消息 |
|------|------|------|
| 加载配置文件 | INFO | `loaded config from {}` |
| 配置文件不存在 | DEBUG | `config file not found: {}, using defaults` |
| 配置变更 | DEBUG | `[key={}] {} → {}` |
| 配置校验失败 | WARN | `[key={}] invalid value: {}` |

### 4.8 ContextCompressor（Phase 3.5）

| 位置 | 级别 | 消息 |
|------|------|------|
| 压缩触发 | INFO | `[sess={}] compress: {} messages, ~{} tokens` |
| 压缩完成 | DEBUG | `[sess={}] compressed: {} → {} messages, {} → {} tokens` |
| 旧 tool_result 摘要化 | DEBUG | `[sess={}] summarize tool_result [tool={}]` |

---

## 五、性能注意事项

### 5.1 高频路径

以下路径每秒可能触发上百次，**默认用 TRACE**，仅在调试时通过 `set_level(TRACE)` 开启：

- EventBus::publish（同步发布）
- StreamingBuffer::push（每个 token 一次）
- Spinner::tick（每 100ms 一次）
- ChatRenderer::render（每帧一次）

### 5.2 字符串格式化开销

- `LOG_DEBUG` 即使级别不足也会评估 `std::format` 参数（当前实现未做级别短路）
- **建议**：高频路径用 `if (logger->get_level() <= LogLevel::DEBUG)` 包裹
- 或后续给 Logger 增加 `is_enabled(level)` 接口，宏内短路

### 5.3 文件输出

- 默认仅控制台输出；通过 `WORKX_LOG_FILE` 环境变量启用文件输出
- 文件路径默认 `logs/workx.log`，按需轮转（当前未实现，长期技术债 G-2）

---

## 六、初始化与配置

### 6.1 main.cpp 初始化顺序

```cpp
// main.cpp 启动序列
auto logger = Agent::Logger::get_instance();

// 1. 从环境变量读取日志级别
if (auto* lvl = std::getenv("WORKX_LOG_LEVEL")) {
    logger->set_level(parse_log_level(lvl));  // TRACE/DEBUG/INFO/WARN/ERROR/FATAL
} else {
    logger->set_level(LogLevel::INFO);  // 默认 INFO
}

// 2. 启用文件输出（可选）
if (auto* file = std::getenv("WORKX_LOG_FILE")) {
    logger->enable_file_output(file, true);
}

LOG_INFO("WorkX starting, version={}, log_level={}",
         WORKX_VERSION, Detail::to_string(logger->get_level()));
```

### 6.2 测试场景

- 单元测试默认禁用日志（或 set_level(FATAL)），避免噪声
- 测试失败时可在 fixture 中临时 set_level(TRACE) 排查

---

## 七、实施清单（与 ARCH_REFACTOR_PLAN 各 Phase 对齐）

| Phase | 模块 | 埋点任务 |
|-------|------|---------|
| Phase 0 | Logger | 确认宏可用；制定本规范 |
| Phase 0.5 | 测试 | 测试失败时输出 EventBus 队列 / TaskManager 任务列表 |
| Phase 1 | EventBus / TaskManager | 4.1 + 4.2 埋点 |
| Phase 2 | ThreadPool | ThreadPool 入队/出队/异常 |
| Phase 3 | ToolExecutor | 4.5 埋点 |
| Phase 3.5 | ReActLoop / ChatSession / ContextCompressor / HttpClient | 4.3 + 4.4 + 4.6 + 4.8 埋点 |
| Phase 4 | EventBus(增强) / ConfigManager | subscribe/unsubscribe 细化 + 4.7 |
| Phase 6 | 补齐遗漏模块 | PermissionChecker / SSEStreamReader 等 |

---

*文档版本: 1.0*
*创建日期: 2026-07-26*
*作者: WorkX Architecture Refactor*
