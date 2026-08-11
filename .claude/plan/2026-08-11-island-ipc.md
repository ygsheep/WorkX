# Island 灵动岛 IPC 模块实施计划（src/island/）

> 日期：2026-08-11
> 状态：已确认，待实施
> 关联：plan/2026-08-07-whaledock-design.md（WhaleDock GUI 设计文档）
> 范围：仅 workx 仓库侧（src/island/ + 单测 + main 接线）；whale-dock GUI 仓库不在本次范围

---

## 1. 目标

实现设计文档 7.2 中的 workx 侧模块，为独立 GUI（WhaleDock）提供：

- IPC 服务端（named pipe / unix socket），JSONL 双通道（事件流 + 请求/响应）
- EventBus → JSONL 事件桥
- 费用累积（CostAccumulator，StreamDoneEvent token → USD）
- 余额拉取（BalanceFetcher，DeepSeek /user/balance，复用 agent 层 HttpClient）
- 单价表（PricingTable，fallback + 用户 JSON 覆盖）
- 注册文件（RegistryWriter，~/.workx/island.registry）
- 单元测试 + main.cpp 接线

## 2. 已确认决策（用户）

1. **IPC 传输层：阻塞 I/O + 线程**（不用 asio，零新增依赖；单客户端场景足够）
2. **默认启用策略：新增 `island.enabled` 配置开关**（默认 true；仅 deepseek + api_key 非空时启用 BalanceFetcher）

## 3. 与设计文档的偏差（有理有据，GUI 端尚未实现，协议以实际事件为准）

| 设计文档假设 | 实际（workx 现状） | 处理 |
|---|---|---|
| SessionStartEvent/ThinkingStartedEvent/TokenStatsEvent 等事件 | **不存在**。实际事件：UserInputEvent / StreamTokenEvent / StreamDoneEvent（全量 token 字段）/ ToolCallEvent / ToolResultEvent / AgentDoneEvent / StreamErrorEvent / CompactionPausedEvent / CacheDiagnosticsEvent | 事件映射按实际事件（见 §5） |
| tool_call/tool_result 含 duration_ms/started_at | ToolCallEvent/ToolResultEvent **无时间字段**（不改核心事件，surgical） | 协议只输出实际字段 |
| asio 异步 I/O | 仓库无 asio | 阻塞 I/O + 线程（用户确认） |
| BalanceFetcher 建在 agent 层 | HttpClient 在 agent/api/remote | 复用 HttpClient，main 注入 api_key/base_url |
| TUI 端 CostAccumulator 订阅 TokenStatsEvent(delta) | token 数据在 StreamDoneEvent（每次 LLM 调用发布一次，天然增量语义） | 订阅 StreamDoneEvent 折算 |
| island.registry 位于 ~/.workx/ | app_config 无此目录 | registry 路径平台化：%USERPROFILE%/.workx 或 $HOME/.workx |

## 4. 层归属与 CMake

- 新增**独立静态库** `workx_island`（src/island/CMakeLists.txt），PUBLIC 链接 workx_agent（http_client 头暴露）
- 不安装、不加入 WORKX_PUBLIC_HEADERS（GUI 通过 IPC 通信，不链接 workx_agent）
- src/CMakeLists.txt：`add_subdirectory(island)`；workx 可执行目标 PRIVATE 链接 workx_island
- 新增 layer_boundary 规则：src/island 禁止 include tui/app/example（新测试文件）

## 5. 事件映射表（EventBus → JSONL event）

| JSONL event type | EventBus 事件 | data 字段 |
|---|---|---|
| `task_started` | UserInputEvent（非本地命令） | {text(截断预览)} |
| `thinking_delta` | StreamTokenEvent（reasoning_delta 非空） | {delta_text} |
| `message_delta` | StreamTokenEvent（content_delta 非空且非 thinking） | {delta_text} |
| `tool_call` | ToolCallEvent | {call_id, tool_name, tool_type, arguments} |
| `tool_result` | ToolResultEvent | {call_id, tool_name, is_error, result_preview} |
| `llm_done` | StreamDoneEvent | {tokens:{input,output,cache_read(input 字段合并为 cache_read_input_tokens+cache_creation?)…}, was_interrupted} |
| `agent_done` | AgentDoneEvent | {total_steps, total_tool_calls, total_duration_ms} |
| `error` | StreamErrorEvent | {message, retryable} |
| `compaction_paused` | CompactionPausedEvent | {paused, ratio, consecutive_compacts, notice} |
| `cache_diag` | CacheDiagnosticsEvent | {prefix_changed, cache_hit_tokens, cache_miss_tokens, reasons} |
| `balance_updated` | island::BalanceUpdatedEvent（fetcher 发布） | {balance_usd, cny_balance, fetched_at, source, error?} |
| `cost_updated` | island::CostUpdatedEvent（accumulator 发布） | {task_cost, session_cost, breakdown{input_usd,output_usd,cache_read_usd,cache_write_usd}, is_estimated, model} |

费用折算（StreamDoneEvent → USD）：
- input = prompt_cache_miss_tokens × input_per_1m（cache miss 即写缓存价）
- cache_read = prompt_cache_hit_tokens × cache_read_per_1m
- output = generated_tokens × output_per_1m
- 模型未知 → deepseek-chat fallback + is_estimated=true

## 6. 模块与文件

```
src/island/
├── CMakeLists.txt                  # workx_island 静态库
├── jsonl_protocol.h/.cpp           # 信封编解码 {kind,type,seq,ts,id,data}
├── pricing_table.h/.cpp            # 单价表：fallback + pricing.json 覆盖
├── cost_accumulator.h/.cpp         # StreamDoneEvent→USD，task/session 清零
├── balance_fetcher.h/.cpp          # 10min 定时 + trigger_refresh（HttpClient，15s 超时）
├── events.h                        # island::BalanceUpdatedEvent / CostUpdatedEvent / CostSnapshot
├── registry_writer.h/.cpp          # ~/.workx/island.registry 原子写（tmp+rename）
├── island_event_bridge.h/.cpp      # EventBus 订阅 → JSONL → server 队列
├── island_server.h/.cpp            # accept 线程 + 连接线程 + seq 环形缓冲(1024) + 请求分发
└── ipc/
    ├── itransport.h                # ITransport 接口 + default_endpoint(pid)
    ├── platform_transport.h        # create_listener/create_connector 工厂（平台分支）
    ├── transport_win32.cpp         # CreateNamedPipe/ConnectNamedPipe
    └── transport_posix.cpp         # AF_UNIX socket
```

请求处理（server 内回调注入，main 接线）：
- hello{last_seq} → 会话快照 + 回放 seq>last_seq
- ping → {pong, tui_pid} + 更新 last_heartbeat → 重写 registry
- get_model_pricing → pricing 表
- refresh_balance → fetcher.trigger_refresh() + wait_for(3s) 返回最新值
- get_session_summary → CostAccumulator 快照 + 会话统计（回调注入）

## 7. main.cpp 接线

```
1. keys::ISLAND_ENABLED = "island.enabled"（默认 true）+ island.usd_cny_rate（默认 7.2）注册 schema
2. island.enabled=true 时：
   - island::IslandServer server(bus)；server.start()（写 registry）
   - island::CostAccumulator acc(bus, pricing, model_name)
   - island::BalanceFetcher fetcher（api_key 非空且 remote_url 含 deepseek 时创建）
   - island::IslandEventBridge bridge(bus, server)
   - server 请求回调接线（refresh_balance → fetcher）
3. 退出：fetcher.stop() → server.stop()（移除 registry）
```

## 8. 测试计划（tests/unit/island/，Catch2）

| 文件 | 覆盖 |
|---|---|
| test_jsonl_protocol.cpp | 编解码往返 / parse 容错 / seq 单调 |
| test_pricing_table.cpp | 精确匹配 / fallback / 未知模型降级 / JSON 覆盖 |
| test_cost_accumulator.cpp | delta 折算 / task 清零 / session 累计 / is_estimated / 连续任务 |
| test_registry_writer.cpp | 写读 / 更新 / 移除 / 损坏容错 |
| test_ipc_transport.cpp | Win pipe 回环：listen/connect/write/read/close/重连（POSIX 分支同理） |
| test_balance_fetcher.cpp | 请求构建（header/超时 15s）/ parse_balance_response 纯函数（成功/401/坏 JSON/空 infos） |
| test_island_event_bridge.cpp | 各事件 → JSONL 字段断言 |
| test_island_server.cpp | 端到端：hello 握手 / 事件推送 / ping-pong / refresh_balance / 断线重连回放 last_seq / 多请求 |
| test_layer_boundary.cpp（island） | src/island 禁止 include tui/app/example |

测试客户端：tests/unit/island/ 内实现简单 IpcTestClient（基于 ITransport connect + JSONL 读写）。

## 9. 验证步骤

1. `cmake --preset default` → `cmake --build build --config Debug --target workx_unit_tests` → `ctest --test-dir build -C Debug --output-on-failure`
2. 全量单测（含既有 layer_boundary / consumer 不回归）

## 10. 交付流程

1. 实现 + 测试绿
2. `gh issue create`（tracking issue，引用设计文档）
3. 分支 `feature/island-ipc` 从 develop 切出 → 提交 → push → `gh pr create`（勾连 issue）