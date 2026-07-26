# 长期技术债登记

> 本文档记录架构重构（Phase 1-6）完成后遗留的技术债，按优先级与影响范围分类。
> 每项包含：现象、影响、建议方案、参考代码位置。

---

## L 类：生命周期与指针安全

### L-1：raw pointer 系统性替换为 `std::reference_wrapper` 或 `not_null<T*>`

**现象**：`ChatSession::backend()` 返回 `IBackend*` 裸指针，调用方需自行判空；`ToolExecutor` 持有 `ToolRegistry*` 裸指针等。

**影响**：低。当前所有裸指针生命周期由 owner 保证，无悬挂风险，但 API 表达力弱。

**建议方案**：
- 引入 `gsl::not_null<T*>`（或自实现）标注非空契约
- 返回 `std::reference_wrapper<T>` 替代 `T&` 返回值
- 真正可空指针用 `std::optional<std::reference_wrapper<T>>` 或 `T*` 保留

**参考位置**：
- `src/agent/core/chat_session.h:108` `IBackend* backend() const`
- `src/agent/tool/executor.h` ToolRegistry 持有方式

### L-2：`m_provider.get()` 在多线程下访问 `unique_ptr` 内部指针

**现象**：`ChatSession::subscribe_interrupt` 捕获 `this`，回调中访问 `m_provider`；析构与回调并发时可能 race。

**影响**：中。当前 interrupt 事件触发频率低，实际未崩溃，但 TSan 会报告。

**建议方案**：
- `m_provider` 改为 `std::shared_ptr<ICompletionProvider>`，回调捕获 `weak_ptr` 后 `lock()` 校验存活
- 或在 ChatSession 析构前先 `unsubscribe_interrupt()` 确保回调不再触发

### L-3：`StreamSession::m_multi` 裸指针跨线程访问

**现象**：`StreamSession` 持有 `CURLM* m_multi`（来自 `HttpClient::Impl`），析构时若 `m_multi` 已被 `Impl::shutdown` 置空则跳过 remove_handle。代码已加防御注释，但仍是裸指针 + 原子标志的弱约定。

**影响**：低。当前代码路径已用 `m_added_to_multi.load() && m_multi` 双重判断，无实际崩溃。

**建议方案**：
- 让 `StreamSession` 持有 `std::shared_ptr<Impl>` 或弱引用，避免裸指针生命周期依赖
- 或将 StreamSession 完全移入 Impl 内部类，访问控制更清晰

---

## H 类：HTTP 客户端健壮性

### H-1：连接池缺失

**现象**：`HttpClient::get` 每次调用 `curl_easy_init` + `curl_easy_cleanup`，无连接复用。

**影响**：中。HTTPS 握手开销大，高频请求场景延迟显著。

**建议方案**：
- `HttpClient` 内部维护 `curl_easy_handle` 池（按 host 复用）
- `curl_multi` 已有连接复用，但同步 `get` 路径未利用

**参考位置**：`src/agent/api/remote/http_client.cpp:96` `HttpClient::get`

### H-2：流式传输无总时长超时

**现象**：`async_post_stream` 仅设 `CURLOPT_LOW_SPEED_LIMIT/TIME`（空闲超时），无总时长上限。长响应（reasoning model 60s+）正常，但若 LLM 持续缓慢吐字则永不超时。

**影响**：中。理论可被恶意服务器耗尽客户端资源。

**建议方案**：
- 添加可选的 `total_timeout_ms` 参数，默认 5 分钟
- 通过 `CURLOPT_TIMEOUT_MS` 设置（注意：会覆盖流式语义，需用 `CURLOPT_MAX_RECV_SPEED_*` 配合）

### H-3：重试逻辑分散，无统一策略

**现象**：`ChatSession::run_completion` 有重试逻辑，`HttpClient` 无重试，`RemoteBackend` 无重试。各层重试策略不一致，可能叠加导致超长延迟。

**影响**：中。重试风暴时延迟不可控。

**建议方案**：
- 抽取 `RetryPolicy` 类型（最大次数 / 退避策略 / 可重试错误码白名单）
- 在 HttpClient 层统一执行（仅对幂等 GET / 网络错误重试，4xx 不重试）
- 上层 ChatSession 只在 LLM 应用层错误时重试

---

## E 类：错误码与执行结果语义

### E-5：`ExecutionResult` 字段语义模糊

**现象**：`ExecutionResult` 同时承载成功/失败状态，字段 `success`、`error_message`、`output` 语义重叠（成功时 output 有效，失败时 error_message 有效，但无类型保护）。

**影响**：低。当前调用方手动判断 `success`，无 bug 但易出错。

**建议方案**：
- 改用 `Result<Output, Error>` 模板（已有 `core/utils/result.h`）
- 或 `std::variant<SuccessData, ErrorData>` 强制模式匹配

### E-6：`HttpResponse` 错误码与 HTTP 状态码混淆

**现象**：`HttpResponse::error` 是 curl 错误字符串，`status_code` 是 HTTP 状态码。两者独立，但调用方常误以为 `error.empty()` 等价于 HTTP 2xx。

**影响**：低。当前代码已分两步检查，但语义可改进。

**建议方案**：
- 拆分为 `CurlError` + `HttpStatusCode` 两个明确概念
- 或提供 `is_success()` 便捷方法（`error.empty() && 200 <= status_code < 300`）

---

## D 类：DI 化未完成项（Phase 4 暂缓）

### D-2：Terminal/ChatRenderer/Client 仍依赖 `EventBus::instance()`

**现象**：架构验收 7.2 标准要求 `EventBus::instance()` 仅在 `core/events/` 和 `main.cpp` 出现，实际仍有 8 处文件使用：`tui/core/terminal.cpp`、`tui/render/chat_renderer.cpp`、`agent/api/client.cpp`、`agent/api/remote/remote_backend.cpp`。

**影响**：中。这些组件无法注入 Mock EventBus，单元测试覆盖率受限。

**建议方案**：
- Terminal 构造函数注入 `IEventBus&`，main.cpp 传入
- ChatRenderer 改为 `publish` 通过 Terminal 代理，或直接注入
- Client / RemoteBackend 注入 `IEventBus&`（与 ChatSession 同模式）

### D-3：Terminal/SetupWizard/ModelSelector/工具内部仍依赖 `ConfigManager::instance()`

**现象**：`ConfigManager::instance()` 在 9 处文件使用，包括 `tui/setup/setup_wizard.cpp`、`app/ui/model_selector.cpp`、`agent/tool/FileReadTool/file_read_tool.cpp` 等。

**影响**：中。工具内部直接读全局配置，无法在测试中注入 Mock 配置。

**建议方案**：
- SetupWizard / ModelSelector 构造函数注入 `IConfigManager&`
- 工具内部配置读取改为通过 ToolContext 传递（避免每次调用都访问单例）

### D-4：`TaskManager::instance()` 在 Terminal 中残留

**现象**：`tui/core/terminal.cpp:98` 调用 `TaskManager::instance().update()` 每帧清理任务。

**影响**：低。Terminal 是 TUI 顶层组件，DI 化收益有限。

**建议方案**：Terminal 构造函数注入 `ITaskManager&`，main.cpp 传入。

---

## T 类：测试覆盖与质量

### T-4：MockConfigManager / MockEventBus / MockTaskManager 缺失

**现象**：Phase 4 已完成接口抽取（IEventBus / IConfigManager / ITaskManager），但单元测试中无 Mock 实现，仍依赖单例。

**影响**：中。无法隔离测试 ChatSession / TaskManager 的事件发布行为。

**建议方案**：在 `tests/unit/helpers/` 下新增 `mock_event_bus.h` / `mock_config_manager.h` / `mock_task_manager.h`，使用 GoogleMock 或手写记录式 Mock。

### T-5：性能基准测试缺失

**现象**：Phase 6 计划补 token 吞吐 / 工具调用延迟 / 并行 vs 串行基准，但 Catch2 单元测试框架不适合微基准。

**影响**：低。当前无回归检测手段，但功能正确。

**建议方案**：
- 引入 `Catch2 benchmark` 宏（`BENCHMARK`）做基础微基准
- 或单独的 `tests/benchmark/` 目录用 google-benchmark
- 关键场景：10 并发 BashTool / 1000 token 流式吞吐 / ReAct 5 轮循环

### T-6：Linux 平台编译未验证

**现象**：Phase 5 CMake 模块化仅在 Windows 验证，Linux 路径（`start_python_server_posix` 等）逻辑写了但未实测。

**影响**：中。CI Linux 节点可能因路径或 fork 行为差异失败。

**建议方案**：CI 添加 Linux 构建任务（Ubuntu 22.04 + gcc 11 + Catch2 v3）。

---

## I 类：集成测试覆盖

### I-1：LM Studio LLM 推理测试需手动启动

**现象**：`test_client.cpp` 9 个 LLM 推理测试用例需手动启动 LM Studio 才能跑。

**影响**：低。这些测试本质需要真实 LLM，无法在 CI 自动化。

**建议方案**：
- CI 跳过 `[integration]` 标签的 LLM 测试
- 文档说明本地运行方式：`set LM_STUDIO_BASE_URL=http://127.0.0.1:1234 && workx_integration_tests.exe`

### I-2：AutoTestServer 在 Linux 上未实测

**现象**：`start_python_server_posix` 用 fork + pipe 实现，逻辑正确但未在 Linux 实测。

**影响**：中。Linux CI 可能因 pipe 缓冲或信号处理差异失败。

**建议方案**：见 T-6。

---

## 优先级建议

| 优先级 | 项 | 理由 |
|--------|-----|------|
| P0 | T-6 + I-2 | Linux CI 是后续所有验证的基础 |
| P1 | D-2 + D-3 + D-4 | 完成 DI 化才能解锁 T-4（Mock 测试） |
| P1 | T-4 | Mock 测试是质量护栏 |
| P2 | H-2 + H-3 | HTTP 客户端健壮性，影响生产稳定性 |
| P2 | T-5 | 性能基准用于检测回归 |
| P3 | L-1 + L-2 + L-3 | 代码质量改进，无紧迫性 |
| P3 | E-5 + E-6 | 类型安全改进 |
| P3 | H-1 | 连接池优化，性能提升 |
| P3 | I-1 | 文档完善 |
