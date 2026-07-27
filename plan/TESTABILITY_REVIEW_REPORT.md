# 架构可测试性审查报告 — src/agent · src/core · src/tui

> **审查日期：** 2026-07-27
> **审查方法：** `.skills/qa-skills/skills/ai-qa-review` Testability Analysis 路径
> **审查假设：** 当前架构存在问题，逐项找证据
> **覆盖维度：** 依赖注入 (DI) / 副作用隔离 / 纯函数提取 / 接口隔离 + 跨模块架构问题
> **审查范围：** 26 个核心头文件 + 关键实现文件（chat_session / react_loop / client / event_bus / config_manager / task_manager / terminal / chat_renderer / factory / remote_backend / executor / registry / context 等）

---

## 总体诊断

代码已有 DI 意识（`ITaskManager` / `IEventBus` / `IConfigManager` 接口存在），但 DI **未贯穿到底**——单例作为默认实参、关键业务类直接调用单例、core 反向依赖 agent、I/O 与纯计算耦合等问题普遍存在。共发现 **24 处可测试性 / 架构缺陷**，按严重程度分级如下。

| 严重级别 | 数量 | 说明 |
|---------|------|------|
| 🔴 HIGH   | 11 | 阻塞测试，掩盖 bug |
| 🟡 MEDIUM | 8  | 增加维护成本，掩盖设计缺陷 |
| 🟢 LOW    | 5  | 代码质量，可延后处理 |

---

## 🔴 HIGH（11 项 — 阻塞测试，掩盖 bug）

### H-1. RemoteBackend 直接调用 `EventBus::instance()`，完全绕过 DI

- **维度：** 依赖注入
- **What：** [src/agent/api/remote/remote_backend.cpp:65](file:///d:/develop/Workspace/workx/src/agent/api/remote/remote_backend.cpp#L65) 和 [src/agent/api/remote/remote_backend.cpp:87](file:///d:/develop/Workspace/workx/src/agent/api/remote/remote_backend.cpp#L87) 在 `initialize()` / `shutdown()` 中直接 `EventBus::instance().publish_async(BackendStatusEvent{...})`。[src/agent/api/remote/remote_backend.h:24](file:///d:/develop/Workspace/workx/src/agent/api/remote/remote_backend.h#L24) 构造函数无 EventBus 参数。
- **Why：** 任何对 RemoteBackend 的单元测试都会污染全局 EventBus，测试间状态串扰。无法注入 MockEventBus 验证 publish 的内容/顺序。这是 D-4 DI 工作在 RemoteBackend 这一关键业务类上的破口。
- **Fix：** RemoteBackend 构造增加 `IEventBus* event_bus = nullptr`（保持向后兼容），缓存为成员；发布时通过成员而非单例。或在 BackendConfig 中携带 event_bus 引用。

### H-2. `EventGuard<T>` 模板硬编码 `EventBus::instance()`

- **维度：** 依赖注入
- **What：** [src/core/events/event_bus.h:191](file:///d:/develop/Workspace/workx/src/core/events/event_bus.h#L191), [src/core/events/event_bus.h:195](file:///d:/develop/Workspace/workx/src/core/events/event_bus.h#L195), [src/core/events/event_bus.h:209](file:///d:/develop/Workspace/workx/src/core/events/event_bus.h#L209) 三处地方在模板构造/析构/移动赋值中硬编码 `EventBus::instance()`。
- **Why：** 即使业务代码注入了 MockEventBus，使用 `EventGuard<T>` 的代码路径仍走真实单例，造成"测试通过但生产 bug"的假象。这是 DI 抽象的破口——一旦代码引用 EventGuard，DI 链即被切断。
- **Fix：** 删除 `EventGuard<T>` 模板（已被 `EventToken + subscribe()` 模式取代，功能重复），或改造为接受 `IEventBus&` 构造参数。优先删除。

### H-3. `core/events/events.h` 反向依赖 `agent/tool/tool_kind.h`

- **维度：** 跨模块架构 — 分层越界
- **What：** [src/core/events/events.h:24](file:///d:/develop/Workspace/workx/src/core/events/events.h#L24) `#include "agent/tool/tool_kind.h"`，因为 `ToolCallEvent.tool_type` 字段使用了 `agent::tool::ToolType` 枚举。注释承认 "ToolType 枚举位于 agent/tool/tool_kind.h"。
- **Why：** core 基础设施层依赖 agent 业务层，违反分层。任何 core 改动会触发 agent 重编；agent 改 `tool_kind.h` 会冲击 core。这是分层污染的种子，未来会扩散为循环依赖。
- **Fix：** 把 `ToolType` 枚举迁移到 `core/events/events.h` 或新建 `core/tool_kind.h`，agent 反向 include core。或更激进：`ToolCallEvent` 不带 `tool_type`，由订阅方根据 `tool_name` 自行推断（已有 `infer_tool_type()` 纯函数在 [src/agent/core/chat_session.cpp:32](file:///d:/develop/Workspace/workx/src/agent/core/chat_session.cpp#L32)）。

### H-4. 构造函数默认实参 `= XXX::instance()` 制造"伪 DI"

- **维度：** 依赖注入
- **What：** 多处构造函数用单例作为默认实参：
  - [src/agent/core/chat_session.h:73-75](file:///d:/develop/Workspace/workx/src/agent/core/chat_session.h#L73-L75) ChatSession 三件套默认实参
  - [src/agent/api/client.h:156](file:///d:/develop/Workspace/workx/src/agent/api/client.h#L156) Client 的 task_manager
  - [src/core/task/task_manager.h:222](file:///d:/develop/Workspace/workx/src/core/task/task_manager.h#L222) TaskManager 默认 EventBus
  - [src/core/config/config_manager.h:159](file:///d:/develop/Workspace/workx/src/core/config/config_manager.h#L159) ConfigScope 默认 ConfigManager
  - [src/tui/core/terminal.h:66-69](file:///d:/develop/Workspace/workx/src/tui/core/terminal.h#L66-L69) Terminal 三个 nullptr + 回退
  - [src/agent/api/client.h:83](file:///d:/develop/Workspace/workx/src/agent/api/client.h#L83) ClientConfig.event_bus = nullptr 回退
- **Why：** 默认实参让调用方"忘记"显式注入 Mock，测试用例会在不知情下走单例。这破坏了 DI 的核心价值：**显式依赖**。`nullptr 回退单例`模式（Terminal/Client）尤其危险——回退路径隐藏在 `event_bus()` accessor 中，调用方看不到。
- **Fix：** 移除所有默认实参，强制显式注入。`main.cpp` / `factory.cpp` 作为唯一组装层允许使用单例；其他层必须显式接收 `IEventBus&`/`IConfigManager&`/`ITaskManager&`。Terminal 的 nullptr 回退路径也应删除。

### H-5. `ToolContext::config_manager()` 回退单例，工具可绕过 DI

- **维度：** 依赖注入
- **What：** [src/agent/tool/context.cpp:18](file:///d:/develop/Workspace/workx/src/agent/tool/context.cpp#L18) `return config_manager_ptr ? *config_manager_ptr : ConfigManager::instance();`。ReActLoop 注入 `IConfigManager*` 时工具走 DI；未注入时走单例。
- **Why：** 工具实现（如 FileReadTool）无法保证配置来自 Mock 还是单例——同一段代码在不同调用路径下行为不同。测试无法隔离工具的配置依赖。
- **Fix：** 强制注入：ReActLoop 构造必传 `IConfigManager&`（非 nullptr），`ToolContext::config_manager()` 不再回退。或返回 `std::optional<IConfigManager*>`，让工具显式处理无配置场景。

### H-6. `ChatSession::save_session` / `load_session` 混合 JSON 序列化与文件 I/O

- **维度：** 副作用隔离
- **What：** [src/agent/core/chat_session.cpp:439](file:///d:/develop/Workspace/workx/src/agent/core/chat_session.cpp#L439) `std::ofstream file(path); file << j.dump(2);`；[src/agent/core/chat_session.cpp:464](file:///d:/develop/Workspace/workx/src/agent/core/chat_session.cpp#L464) `std::ifstream file(path); file >> j;`。序列化逻辑（[src/agent/core/chat_session.cpp:425-431](file:///d:/develop/Workspace/workx/src/agent/core/chat_session.cpp#L425-L431)）和反序列化（[src/agent/core/chat_session.cpp:482-503](file:///d:/develop/Workspace/workx/src/agent/core/chat_session.cpp#L482-L503)）与文件读写耦合在同一函数。
- **Why：** 无法对"序列化格式是否正确"做单元测试而不触碰文件系统；无法对"I/O 错误处理"做测试而不创建真实文件。两者混合掩盖了序列化字段顺序/格式错误的回归风险。
- **Fix：** 提取 `nlohmann::json ChatSession::serialize_state() const` 和 `Result<void,std::string> ChatSession::deserialize_state(const nlohmann::json&)` 为纯函数。`save`/`load` 仅做 `serialize_state() → ofstream` / `ifstream → deserialize_state()`。

### H-7. `ChatSession::run_completion` 错误处理混合 5 类关注点

- **维度：** 副作用隔离 / 纯函数提取
- **What：** [src/agent/core/chat_session.cpp:297-340](file:///d:/develop/Workspace/workx/src/agent/core/chat_session.cpp#L297-L340) 中错误处理混合了：①可重试判定（纯逻辑）②退避延迟计算（纯逻辑，已委托 HttpRetryPolicy）③事件发布（I/O）④sleep 等待（I/O）⑤递归重试调用（控制流）。
- **Why：** 无法对"是否应重试 / 退避时长"做纯函数测试。重试策略变更需要修改业务路径。一旦改错，所有工具调用失败时的行为都受影响。
- **Fix：** 抽出 `RetryDecision compute_retry(const ReActResult&, const HttpRetryPolicy&, int attempt)` 纯函数返回 `enum {Continue, Stop, Sleep} + delay_ms`，`run_completion` 仅按决策执行 I/O。

### H-8. `IBackend` 接口未彻底拆分，`session->backend()` 仍返回 `IBackend*`

- **维度：** 接口隔离
- **What：** [src/agent/api/i_backend.h:28](file:///d:/develop/Workspace/workx/src/agent/api/i_backend.h#L28) `class IBackend : public ICompletionProvider, public IBackendAdmin`。D-3 拆分了接口但 [src/agent/core/chat_session.h:112](file:///d:/develop/Workspace/workx/src/agent/core/chat_session.h#L112) `IBackend* backend() const` 仍返回完整 `IBackend*`，[src/app/main.cpp:172-184](file:///d:/develop/Workspace/workx/src/app/main.cpp#L172-L184) 通过它调用 `set_model_name` / `list_models`。
- **Why：** ChatSession 内部只用 `ICompletionProvider`，但暴露 `IBackend*` 给外部等于让 UI 层能调用所有 `IBackendAdmin` 方法，包括 `shutdown()`。这绕过了接口隔离的初衷——UI 层可误调用 lifecycle 方法。
- **Fix：** ChatSession 应暴露 `IBackendAdmin* backend_admin()` 或直接返回 `ICompletionProvider*`，UI 层需要管理能力时通过独立的 `IBackendAdmin&` 注入（factory 创建 session 时同时返回 backend_admin 句柄）。

### H-9. ChatSession 析构函数 `sleep_for 50ms` 轮询等待后台任务

- **维度：** 跨模块架构 — 生命周期
- **What：** [src/agent/core/chat_session.cpp:151-154](file:///d:/develop/Workspace/workx/src/agent/core/chat_session.cpp#L151-L154) 析构函数循环 `sleep_for(50ms)` 直到 `task->isRunning()` 为 false 或 30 秒超时。
- **Why：**
  1. 析构时间不可预测（最长 30s 阻塞，主线程卡死）
  2. 轮询空转浪费 CPU
  3. 违反 RAII——析构应通过 condition_variable 唤醒（已有 `m_task_cv` 但未使用）
  4. 如果 TaskManager 在 ChatSession 之前析构，task 句柄失效，行为未定义
- **Fix：** Task 析构时调用 `m_event_bus.publish(TaskFinishedEvent)`，ChatSession 订阅后 `m_task_cv.wait()`。或 ChatSession 析构前显式 `m_task_manager.cancel(task); m_task_manager.wait(task);`。

### H-10. `core/events/events.h` 是全局事件类型大杂烩，违反单一职责

- **维度：** 接口隔离 / 跨模块架构
- **What：** [src/core/events/events.h](file:///d:/develop/Workspace/workx/src/core/events/events.h) 把 13 个事件类型（`UserInputEvent` / `InterruptEvent` / `StreamTokenEvent` / `StreamDoneEvent` / `StreamErrorEvent` / `StepDoneEvent` / `AgentStepEvent` / `ToolCallEvent` / `ToolResultEvent` / `AgentDoneEvent` / `ModelLoadEvent` / `BackendStatusEvent` / `ShutdownEvent`）全部堆在一个头文件。订阅方只要 1 个事件也要 include 整个文件。
- **Why：** 编译耦合：改任意一个事件字段触发全工程重编。命名空间污染。无法做事件命名空间隔离（如 `core::events::system::*` vs `agent::events::stream::*`）。MockEventBus 实现需要知道全部 13 个事件类型。
- **Fix：** 按域拆分：`core/events/system_events.h`（`ShutdownEvent`）、`core/events/stream_events.h`（`Stream*` / `Step*`）、`agent/events/agent_events.h`（`AgentStep` / `AgentDone` / `ToolCall` / `ToolResult`）。或保持单文件但按命名空间分组。

### H-11. `input/processor.h` 直接 `std::ifstream` 读文件

- **维度：** 副作用隔离
- **What：** [src/agent/input/processor.h:97](file:///d:/develop/Workspace/workx/src/agent/input/processor.h#L97) InputProcessor 内 `std::ifstream file(path, std::ios::binary)` 处理 `@file` 引用。
- **Why：** 无法对 @file 注入逻辑做单元测试而不创建临时文件。无法 mock 文件系统错误（权限拒绝、文件不存在、符号链接逃逸）。
- **Fix：** 抽出 `IFileLoader` 接口（`virtual std::string load(const std::string&) = 0;`），InputProcessor 依赖它，生产用 `LocalFileLoader`，测试用 `InMemoryFileLoader`。

---

## 🟡 MEDIUM（8 项 — 增加维护成本，掩盖设计缺陷）

### M-1. `factory.cpp` 中 `create_session` 显式传递三个单例

- **维度：** 依赖注入
- **What：** [src/app/factory.cpp:120-124](file:///d:/develop/Workspace/workx/src/app/factory.cpp#L120-L124) 工厂"可测试"声明，但内部硬编码 `TaskManager::instance()` / `EventBus::instance()` / `ConfigManager::instance()`。
- **Why：** factory.cpp 单元测试无法替换 Mock。`SessionResult create_session(cfg, preset)` 签名暴露了"读 cfg"的依赖，但隐藏了"用三个单例"的依赖——签名骗人。
- **Fix：** `create_session(IConfigManager&, const ProviderPreset*, ITaskManager&, IEventBus&, IConfigManager&)` 显式注入。`main.cpp` 装配层显式传单例。这样 factory 可被 Mock 驱动测试。

### M-2. `app_config.cpp` / `cli_args.cpp` / `model_selector.cpp` / `setup_wizard.cpp` 直接用 `ConfigManager::instance()`

- **维度：** 依赖注入
- **What：**
  - [src/app/config/app_config.cpp:24](file:///d:/develop/Workspace/workx/src/app/config/app_config.cpp#L24)
  - [src/app/config/app_config.cpp:179](file:///d:/develop/Workspace/workx/src/app/config/app_config.cpp#L179)
  - [src/app/config/app_config.cpp:196](file:///d:/develop/Workspace/workx/src/app/config/app_config.cpp#L196)
  - [src/app/config/cli_args.cpp:23](file:///d:/develop/Workspace/workx/src/app/config/cli_args.cpp#L23)
  - [src/app/ui/model_selector.cpp:44](file:///d:/develop/Workspace/workx/src/app/ui/model_selector.cpp#L44)
  - [src/tui/setup/setup_wizard.cpp:395](file:///d:/develop/Workspace/workx/src/tui/setup/setup_wizard.cpp#L395)

  `IConfigManager` 接口存在但未注入。
- **Why：** 这些组件不可测试。`register_config_defaults()` 写入的是单例，测试间状态串扰。
- **Fix：** 所有这些组件接收 `IConfigManager&` 构造参数；`main.cpp` 把 `ConfigManager::instance()` 显式传入。

### M-3. `ToolExecutor::execute` 混合日志 + 业务 + 异常处理

- **维度：** 副作用隔离
- **What：** [src/agent/tool/executor.h:103-198](file:///d:/develop/Workspace/workx/src/agent/tool/executor.h#L103-L198) `execute()` 在每次工具调用前后调用 `LOG_DEBUG/LOG_INFO/LOG_WARN/LOG_ERROR`，5 个 catch 分支，2 个 truncate 分支。150 行方法做了 5 件事。
- **Why：** 无法对"工具执行失败时是否正确返回 ExecutionResult"做测试而不污染日志输出。日志副作用使测试输出嘈杂。
- **Fix：** 把 `execute()` 拆为：
  1. `lookup_tool()` 纯查找
  2. `run_with_safety()` try-catch 包装
  3. `finalize_result()` 截断+日志

  日志通过 `IToolExecutorObserver` 注入或返回 `ExecutionTrace` 结构让调用方决定是否记录。

### M-4. `IConfigManager` 接口过宽，调用方依赖整个接口

- **维度：** 接口隔离
- **What：** [src/core/config/i_config_manager.h](file:///d:/develop/Workspace/workx/src/core/config/i_config_manager.h) 暴露 `has/get/get_value/set_value/load_from_file/save_to_file/get_all_keys`。但：
  - ConfigScope 仅用 `set/get/get_or`
  - setup_wizard 仅用 `set/save_to_file`
  - ChatSession 仅用 `get/get_or/has`
- **Why：** Mock 实现必须实现全部方法，即使是无关的。修改 `IConfigManager`（如增加 `remove()`）会冲击所有调用方。
- **Fix：** 拆为 `IConfigReader`（`has/get/get_or`）+ `IConfigWriter`（`set`）+ `IConfigPersistence`（`load/save`）。ConfigScope 依赖 Reader+Writer，setup_wizard 依赖 Writer+Persistence。

### M-5. `ITool` 接口过宽，工具实现必须实现 7 个方法

- **维度：** 接口隔离
- **What：** `itool.h` 包含 `name/description/input_schema/prompt/check_permissions/validate_input/call`。但：
  - ToolExecutor 只用 `find_by_name + call`
  - UI 层只用 `name/description`
  - schema 生成只用 `input_schema`
- **Why：** 新增工具需实现 7 个方法，多数会写成空实现或重复代码。`check_permissions` / `validate_input` 实际上应由独立的 `IPermissionChecker` / `IInputValidator` 做。
- **Fix：** 拆 ITool 为：
  - `IToolMetadata`（`name/desc/schema/prompt`）
  - `IToolCallable`（`call`）
  - `IToolGuard`（`check_permissions/validate_input`）

  默认 `IToolGuard` 实现提供"无权限检查 + JSON schema 验证"。

### M-6. `Task` 用 `friend TaskManager` + `enable_shared_from_this` 突破封装

- **维度：** 跨模块架构 — 封装
- **What：** [src/core/task/task_manager.h:45](file:///d:/develop/Workspace/workx/src/core/task/task_manager.h#L45) `class Task : public std::enable_shared_from_this<Task>`，[src/core/task/task_manager.h:168](file:///d:/develop/Workspace/workx/src/core/task/task_manager.h#L168) `friend class TaskManager;`。TaskManager 直接调用 `Task::execute` / `markCompleted` / `markFailed` 私有方法。
- **Why：** Task 的状态机无法独立测试——必须通过 TaskManager 才能 trigger。状态转换的内部不变量被 friend 绕开，未来 TaskManager 改动可能破坏 Task 不变量。
- **Fix：** `Task::execute()` 改为 public（或 protected + Task 暴露 `transition_to_xxx()` 公共接口），删除 friend。或把状态机提取为 `TaskStateMachine` 纯类独立测试。

### M-7. `RemoteBackend` 的 `m_active_mutex` + `m_generating` 双状态可能不一致

- **维度：** 跨模块架构 — 共享可变状态
- **What：** [src/agent/api/remote/remote_backend.h:45-46](file:///d:/develop/Workspace/workx/src/agent/api/remote/remote_backend.h#L45-L46) `std::atomic<bool> m_ready` / `m_generating` 独立于 `m_active_mutex` + `m_active_reader`。`interrupt()` 置 `m_active_reader=nullptr` 但 `m_generating` 由 `submit_completion` 在锁外置位。
- **Why：** 状态机漏洞：`interrupt` 后 `m_active_reader=nullptr`，但 `m_generating` 可能仍为 true，UI 显示"正在生成"实际已停止。
- **Fix：** 状态合并为单一 enum `enum class BackendState { Idle, Connecting, Streaming, Interrupted }`，受 `m_active_mutex` 保护。

### M-8. `EventBus::publish_async` + `process_async_events` 无强制驱动约束

- **维度：** 跨模块架构 — 协议模糊
- **What：** [src/core/events/event_bus.h:95-124](file:///d:/develop/Workspace/workx/src/core/events/event_bus.h#L95-L124) `publish_async` 入队后，`process_async_events` 由谁调用、调用频率多少没有约束。如果调用方忘记轮询，事件丢失或延迟。
- **Why：** 异步事件可靠性低。无法在测试中验证"事件最终被处理"。
- **Fix：**
  1. 强制集成：`Terminal::run()` 主循环必须调用 `process_async_events`（已有但无文档约束）
  2. 提供 `EventBus::drain_async(timeout)` 给测试用
  3. 监控 `m_async_queue` 积压并发布 `BackpressureEvent`

---

## 🟢 LOW（5 项 — 代码质量，可延后处理）

### L-1. `infer_tool_type` 放匿名命名空间，无法复用

- **维度：** 纯函数提取
- **What：** [src/agent/core/chat_session.cpp:32-41](file:///d:/develop/Workspace/workx/src/agent/core/chat_session.cpp#L32-L41) 已正确提取为纯函数，但放匿名 namespace，未在头文件声明，无法被其他模块或测试复用。
- **Fix：** 移到 `tool_kind.h/.cpp` 作为公共纯函数 `tool::ToolType infer_tool_type(std::string_view name)`。

### L-2. `truncate_result` in-out 参数违反纯函数原则

- **维度：** 纯函数提取
- **What：** [src/agent/tool/executor.h:72-82](file:///d:/develop/Workspace/workx/src/agent/tool/executor.h#L72-L82) `inline bool truncate_result(std::string& text, ...)` 直接修改入参，返回是否截断。本应是纯函数但有副作用。
- **Fix：** 改为 `inline std::pair<std::string, bool> truncate_result(std::string_view text, size_t max_length)`，返回新串 + 是否截断。

### L-3. `Client::compute_backoff_delay_ms` 重复封装 `HttpRetryPolicy::delay_ms`

- **维度：** 纯函数提取
- **What：** [src/agent/api/client.h:175](file:///d:/develop/Workspace/workx/src/agent/api/client.h#L175) `int64_t compute_backoff_delay_ms(int attempt) const` 是成员方法，但 `HttpRetryPolicy::delay_ms` 已经是静态纯函数。Client 这一层是冗余包装。
- **Fix：** 删除 `Client::compute_backoff_delay_ms`，直接调用 `HttpRetryPolicy::delay_ms()`。

### L-4. `main.cpp:54` 在 `namespace agent` 中 `using namespace tui`

- **维度：** 跨模块架构 — 命名空间污染
- **What：** [src/app/main.cpp:54](file:///d:/develop/Workspace/workx/src/app/main.cpp#L54) `using namespace tui;` 在 `namespace agent` 中混入 TUI 类型。注释说"P0: tui→agent 类型引用过渡方案"。
- **Fix：** 改为 `namespace wt = ::tui;` 别名，或显式 `tui::Terminal`。

### L-5. `EventBus::clear_for_test()` 是补丁式 API

- **维度：** 跨模块架构 — 测试夹具
- **What：** [src/core/config/config_manager.h:139](file:///d:/develop/Workspace/workx/src/core/config/config_manager.h#L139) `void clear_for_test();` 在生产接口中暴露测试专用方法。
- **Fix：** 改为 `friend class ConfigManagerTestAccess` 或独立 `ConfigManagerTestFixture`，生产接口不含测试方法。

---

## 修复优先级建议

| 优先级 | 项 | 影响范围 | 建议时机 |
|---|---|---|---|
| P0 | H-1, H-2, H-3, H-4 | DI 抽象破口 + 分层越界，不修则后续所有 DI 工作前功尽弃 | 立即 |
| P1 | H-5, H-6, H-7, H-8 | 业务路径可测试性，影响核心 ChatSession / RemoteBackend | 本迭代 |
| P2 | H-9, H-10, H-11, M-1, M-2 | 生命周期与 app/tui 层 DI 贯穿 | 下一迭代 |
| P3 | M-3 ~ M-8 | 接口隔离与状态机一致性 | 长期 |
| P4 | L-1 ~ L-5 | 代码质量收尾 | 顺手处理 |

---

## 审查方法论说明

- 本报告基于代码静态审查 + `.skills/qa-skills/skills/ai-qa-review` 描述的 Testability Analysis 维度框架。
- 审查严格遵循 ai-qa-review SKILL.md 的 Quick Route 第三路径（应用代码 testability 分析），用 before/after 形式给出每条 finding 的可测试化重构建议。
- 未执行 mutation testing / 运行测试套件验证，建议后续对 H 级 finding 单独写复现测试用例做客观验证。
- 未覆盖：`tests/` 目录的测试代码本身的可测试性、第三方依赖（Boost.Beast / nlohmann-json / Catch2）的版本风险、构建系统 CMake 模块化。

---

**审查人：** AI Agent (基于 ai-qa-review skill)
**对应文件：** `plan/TESTABILITY_REVIEW_REPORT.md`
