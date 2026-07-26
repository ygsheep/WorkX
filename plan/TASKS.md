# 技术债清理任务清单

> 基于 `TECH_DEBT_REGISTRY.md` 与 `PHASE3_LONG_TERM_REFACTOR.md`，按优先级排序的可执行任务。
> 每项含：目标、改动范围、验收标准、回归检查。

---

## P0：Linux CI 验证（前置）

### TASK-P0-1：Linux 平台编译验证 [Q-3]

**目标**：在 Ubuntu 22.04 + gcc 11 上验证 CMake 模块化构建

**改动范围**：
- 检查 `src/tui/CMakeLists.txt` 平台分支（`platform_win32.cpp` / `platform_posix.cpp`）
- 检查 `tests/integration/test_server_fixture.h` 的 POSIX 分支（fork/pipe）
- 可能的移植问题：`<windows.h>` 包含、`_CRT_SECURE_NO_WARNINGS`、MSVC 特定 pragma

**验收标准**：
- [ ] Ubuntu 22.04 + gcc 11 + Catch2 v3 编译通过
- [ ] 单元测试全部通过（336 cases）
- [ ] 集成测试 Python server 路径通过（6 cases）

**回归检查**：Windows 平台编译 + 测试无变化

---

## P1：低风险快速收益（1 周内）

### TASK-P1-1：Logger 析构 join 修复 [G-2]

**目标**：消除 `~Logger()` 中 `detach()` 的 use-after-free 风险

**改动范围**：`lib/liblogger/logger.h:136-152`

**实施步骤**：
1. 将 `m_writer_thread.detach()` 改为 `m_writer_thread.join()`
2. 确保写线程队列有界（防阻塞），超限丢弃旧日志并 WARN
3. 关闭文件流前确保 `join()` 完成

**验收标准**：
- [ ] `grep "detach" lib/liblogger/` 无匹配
- [ ] 进程退出无崩溃/卡死
- [ ] 日志高负载（1000 条/秒）下不丢日志（除超限丢弃外）

---

### TASK-P1-2：ReActLoop 移动语义删除 [L-2]

**目标**：消除 `ReActLoop` 移动后 `m_provider` 悬空风险

**改动范围**：`src/agent/core/react_loop.h:189-193, :296`

**实施步骤**：
1. 删除 `ReActLoop(ReActLoop&&) = default` 和 `operator=(ReActLoop&&) = default`
2. `m_provider` 改为 `std::reference_wrapper<ICompletionProvider>`
3. 验证 `ChatSession::run_completion()` 中 `ReActLoop loop(...)` 是栈局部变量不移动

**验收标准**：
- [ ] 编译通过（无移动语义使用处）
- [ ] 现有 ReActLoop 单元测试通过

---

### TASK-P1-3：ChatRenderer 生命周期契约 [L-3]

**目标**：文档化 `Terminal*` 生命周期约束

**改动范围**：`src/tui/render/chat_renderer.h:34-67`

**实施步骤**：
1. 构造函数添加 `assert(terminal != nullptr)`
2. 成员变量添加注释 `// Terminal must outlive ChatRenderer`
3. 不改指针类型（PHASE3 判定同层对象由 main 控制）

**验收标准**：
- [ ] 构造函数有 assert
- [ ] 成员注释明确生命周期契约

---

### TASK-P1-4：Client 析构 assert [L-6]

**目标**：补全 PHASE3 建议的防御性 assert

**改动范围**：`src/agent/api/client.cpp:128-134`

**实施步骤**：在 `~Client()` 开头添加 `assert(m_task_manager != nullptr)`

**验收标准**：
- [ ] 析构函数有 assert

---

### TASK-P1-5：Terminal 字段原子化 [T-4]

**目标**：防御性原子化 `m_running` / `m_initialized`

**改动范围**：`src/tui/core/terminal.h:184-185`

**实施步骤**：
1. `bool m_running` → `std::atomic<bool> m_running{false}`
2. `bool m_initialized` → `std::atomic<bool> m_initialized{false}`
3. 检查所有访问处是否需要 `load()/store()`（同线程访问可保持原语义）

**验收标准**：
- [ ] 编译通过
- [ ] TUI 交互无回归

---

### TASK-P1-6：Task time_point static_assert [T-6]

**目标**：文档化 `m_start_time` 的平台假设

**改动范围**：`src/core/task/task_manager.h:146`

**实施步骤**：在 `m_start_time` 声明前添加：
```cpp
static_assert(sizeof(std::chrono::steady_clock::time_point) <= sizeof(uint64_t),
              "time_point must be trivially copyable for atomic-like reads");
```

**验收标准**：
- [ ] static_assert 通过

---

## P2：DI 化与配置系统（2-3 周）

### TASK-P2-1：Terminal/ChatRenderer/Client 注入 IEventBus [D-4]

**目标**：消除 `EventBus::instance()` 在 TUI/Client 层的使用

**改动范围**：8 处文件
- `tui/core/terminal.cpp`
- `tui/render/chat_renderer.cpp`
- `agent/api/client.cpp`
- `agent/api/remote/remote_backend.cpp`

**实施步骤**：
1. 各类构造函数添加 `IEventBus&` 参数，默认值 `EventBus::instance()`（向后兼容）
2. 成员变量改为 `std::reference_wrapper<IEventBus>`
3. `main.cpp` 显式注入

**验收标准**：
- [ ] `grep "EventBus::instance()" src/tui/ src/agent/api/` 无匹配（除默认参数）
- [ ] 全量测试通过

---

### TASK-P2-2：SetupWizard/ModelSelector 注入 IConfigManager [D-5]

**目标**：消除 `ConfigManager::instance()` 在 TUI/工具内部使用

**改动范围**：9 处文件

**实施步骤**：
1. SetupWizard / ModelSelector 构造函数注入 `IConfigManager&`
2. 工具内部配置读取改为通过 `ToolContext` 传递
3. `main.cpp` 显式注入

**验收标准**：
- [ ] `grep "ConfigManager::instance()" src/tui/ src/agent/tool/` 无匹配
- [ ] 全量测试通过

---

### TASK-P2-3：Terminal 注入 ITaskManager [D-6]

**目标**：消除 `Terminal` 中的 `TaskManager::instance()`

**改动范围**：`tui/core/terminal.cpp:98`

**实施步骤**：Terminal 构造函数添加 `ITaskManager&` 参数，`main.cpp` 传入

**验收标准**：
- [ ] `grep "TaskManager::instance()" src/tui/` 无匹配

---

### TASK-P2-4：Mock 实现补充 [Q-1]

**目标**：提供 MockEventBus / MockConfigManager / MockTaskManager

**改动范围**：`tests/unit/helpers/` 新增 `mock_event_bus.h` / `mock_config_manager.h` / `mock_task_manager.h`

**实施步骤**：
1. 手写记录式 Mock（不引入 GoogleMock 依赖）
2. MockEventBus 记录 `publish_async` 调用供测试断言
3. MockConfigManager 支持预设配置值
4. MockTaskManager 支持 `create`/`launch` 的同步执行

**验收标准**：
- [ ] 3 个 Mock 头文件可用
- [ ] 至少 1 个测试用例使用 Mock 替代单例

---

### TASK-P2-5：ConfigSchema 增强 [C-2]

**目标**：配置系统支持类型/范围/枚举校验

**改动范围**：`src/core/config/config_manager.h`

**实施步骤**：
1. 新增 `ConfigSchema` 结构（type/int_range/double_range/enum_values）
2. `ConfigManager::register_schema()` API
3. `set_value()` 时自动校验

**验收标准**：
- [ ] 非法值设置返回错误
- [ ] 现有 `register_meta` 调用迁移到 schema

---

### TASK-P2-6：ConfigScope DI 化 [C-3]

**目标**：`ConfigScope` 持有 `IConfigManager&` 而非单例

**改动范围**：`src/core/config/config_manager.h:91-123`

**验收标准**：
- [ ] `ConfigScope` 构造可注入 `MockConfigManager`

---

### TASK-P2-7：环境变量 schema 绑定 [C-4]

**目标**：环境变量自动加载，集中文档化

**改动范围**：`src/app/config/app_config.cpp:135-161`、`ConfigSchema` 增加 `env_var` 字段

**验收标准**：
- [ ] 7 个环境变量通过 schema 自动加载
- [ ] `workx --dump-config-schema` 输出文档

---

### TASK-P2-8：Logger 命名空间统一 [G-3, G-4]

**目标**：`Agent::Logger` → `agent::log::Logger`，`get_instance()` 返回引用

**改动范围**：`lib/liblogger/logger.h` 全部 + 所有 `LOG_*` 宏使用处

**实施步骤**：
1. 命名空间迁移：`namespace Agent` → `namespace agent::log`
2. 兼容别名：`namespace Agent = agent::log;`
3. `get_instance()` 返回 `Logger&`
4. 修复 `logger.h:344` 的 `DearTs` 注释错配
5. 宏更新为 `agent::log::Logger::get_instance()`

**验收标准**：
- [ ] 编译通过
- [ ] 旧代码通过 alias 兼容
- [ ] 无 `shared_ptr<Logger>` 残留

---

## P3：HTTP 客户端演进（1-2 周）

### TASK-P3-1：HttpRetryPolicy 提取 [H-3]

**目标**：统一重试策略

**改动范围**：新增 `src/agent/api/retry_policy.h`，修改 `RemoteBackend` / `Client` / `ChatSession`

**实施步骤**：
1. 定义 `HttpRetryPolicy`（max_retries/base_delay/max_delay/total_timeout/is_retryable）
2. `RemoteBackend` 构造注入 policy，内部统一执行重试
3. `Client` / `ChatSession` 移除各自重试逻辑，委托给 `RemoteBackend`

**验收标准**：
- [ ] 重试逻辑仅在 `RemoteBackend` 一处
- [ ] 现有重试测试通过

---

### TASK-P3-2：总时长超时 [H-2]

**目标**：StreamSession 支持总时长上限

**改动范围**：`src/agent/api/remote/http_client.cpp`

**实施步骤**：
1. `StreamSession` 增加 `m_total_timeout_ms` + `m_start_time`
2. `poll_loop` 定期检查，超时触发 `cancel()` + `finish_with_error()`
3. 默认 120 秒，可配置

**验收标准**：
- [ ] 慢响应测试：模拟 1 byte/s 持续传输，120 秒后超时
- [ ] 正常响应不受影响

---

### TASK-P3-3：CURLSH 连接共享 [H-1]

**目标**：跨 `HttpClient` 实例共享连接缓存

**改动范围**：`src/agent/api/remote/http_client.cpp:311-322`

**实施步骤**：`Impl` 中使用 `CURLSH* shared_cache()` 共享 `CURL_LOCK_DATA_CONNECT`

**验收标准**：
- [ ] 并发 10 请求总耗时较前无退化
- [ ] 无连接泄漏

---

### TASK-P3-4：URL 解析统一 [H-4]

**目标**：删除 `parse_url` 的 fallback 逻辑

**改动范围**：`src/agent/api/remote/http_client.cpp:29-80`

**验收标准**：
- [ ] 仅一套 CURLU 解析逻辑
- [ ] URL 解析单元测试覆盖各种格式

---

### TASK-P3-5：main.cpp 工厂函数 [D-2]

**目标**：提取 `build_app()` 工厂，`main()` < 20 行

**改动范围**：新增 `src/app/di/factory.h`，重构 `main.cpp`

**验收标准**：
- [ ] `main()` 仅含 `build_app()` + `run()` + 异常处理
- [ ] 组装逻辑集中在 `build_app()`

---

### TASK-P3-6：IBackendAdmin 拆分 [D-3]

**目标**：`IBackend` 拆分为 `ICompletionProvider` + `IBackendAdmin`

**改动范围**：`src/agent/api/i_backend.h`

**验收标准**：
- [ ] `Client` 通过 `IBackendAdmin*` 调用管理接口
- [ ] `ChatSession` 仅依赖 `ICompletionProvider`

---

### TASK-P3-7：错误处理类型安全 [E-5, E-6]

**目标**：`ExecutionResult` 和 `HttpResponse` 语义清晰化

**改动范围**：`src/agent/tool/executor.h`、`src/agent/api/remote/http_client.h`

**验收标准**：
- [ ] `ExecutionResult` 改用 `Result<Output, Error>`
- [ ] `HttpResponse` 提供 `is_success()` 便捷方法

---

## V2：错误处理统一（独立立项）

### TASK-V2-1：Error 类型设计 [E-1]

**目标**：设计统一 `Error` 类型，新代码强制使用 `ResultV2<T>`

**说明**：独立立项 `ARCH_REFACTOR_PLAN_V2.md`，不在本次技术债清理范围内

---

## 执行原则

1. **逐项推进**：每完成一个 TASK 提交一次，不批量改动
2. **测试先行**：每个 TASK 前后运行全量测试（336 cases）
3. **向后兼容**：公共 API 变更保留旧接口 `[[deprecated]]` 转发
4. **日志追踪**：重构过程中补充关键路径 LOG
5. **文档同步**：完成 TASK 后更新 `TECH_DEBT_REGISTRY.md` 状态

---

## 进度追踪

| TASK | 优先级 | 状态 | 完成时间 | Commit |
|------|--------|------|----------|--------|
| P0-1 | P0 | ⬜ | — | — |
| P1-1 | P1 | ⬜ | — | — |
| P1-2 | P1 | ⬜ | — | — |
| P1-3 | P1 | ⬜ | — | — |
| P1-4 | P1 | ⬜ | — | — |
| P1-5 | P1 | ⬜ | — | — |
| P1-6 | P1 | ⬜ | — | — |
| P2-1 | P2 | ⬜ | — | — |
| P2-2 | P2 | ⬜ | — | — |
| P2-3 | P2 | ⬜ | — | — |
| P2-4 | P2 | ⬜ | — | — |
| P2-5 | P2 | ⬜ | — | — |
| P2-6 | P2 | ⬜ | — | — |
| P2-7 | P2 | ⬜ | — | — |
| P2-8 | P2 | ⬜ | — | — |
| P3-1 | P3 | ⬜ | — | — |
| P3-2 | P3 | ⬜ | — | — |
| P3-3 | P3 | ⬜ | — | — |
| P3-4 | P3 | ⬜ | — | — |
| P3-5 | P3 | ⬜ | — | — |
| P3-6 | P3 | ⬜ | — | — |
| P3-7 | P3 | ⬜ | — | — |
