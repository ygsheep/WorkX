# Hook 事件系统（`agent::hook`）

> Issue #50 · 通用 Hook 事件系统。参考 cc（claude-code）的 hook 模型，向 agent
> harness 暴露统一的事件钩子：在工具执行、会话启停、子代理、权限请求等时机拦截
> 或扩展行为。完整设计见 `docs/HOOKS_SYSTEM_DESIGN.md`。

## 核心对象

| 文件 | 职责 |
| --- | --- |
| `hook_event.h` | 事件 / 上下文 / 结果 / 注册定义的类型定义 |
| `hook_match.h` | `if` 条件 matcher（注册时一次性编译） |
| `hook_manager.h/.cpp` | 注册表 + `dispatch` 调度 + 四种执行器 + 进度事件发布 |

## 事件（8 种 `HookEvent`）

| 事件 | 触发时机 | 关联上下文字段 |
| --- | --- | --- |
| `PreToolUse` | 工具执行前（并行执行前同步调度，可拦截） | `tool_name` / `tool_input` |
| `PostToolUse` | 工具执行后 | `tool_name` / `tool_result` / `tool_error` |
| `SessionStart` | 会话装配完成、首条消息前（每实例一次） | `session_id` / `cwd` |
| `SessionEnd` | 会话析构收尾时 | `session_id` / `cwd` / `stop_reason` |
| `Stop` | Agent 停止（统一在循环收尾后触发一次） | `final_answer` / `stop_reason` |
| `SubagentStart` | 子代理 run() 之前（父作用域） | `subagent_id` / `subagent_prompt` |
| `SubagentStop` | 子代理收尾后（父作用域，子级不再重复 Stop） | `subagent_id` / `final_answer` / `stop_reason` |
| `PermissionRequest` | 工具权限决策前（hook 可动态授权/阻断） | `tool_name` / `tool_input` / `request_id` |

## 类型（4 种 `HookType`）

| 类型 | 执行方式 |
| --- | --- |
| `command` | `cmd.exe /d /s /c`（Win）/ `sh -c`（POSIX），`agent::process::exec`，输出截断 64KB |
| `http` | `HttpClient::post_json`（POST URL，自定义 headers + 事件上下文 JSON 体） |
| `prompt` | 调 `ICompletionProvider` 单次推理（temperature 0，max_tokens 200），JSON 判定 |
| `agent` | **未实现**（`run_agent` 返回 `not yet implemented`；需工具注册表白名单） |

`blockingError` / `preventContinuation` 直接作用于当前 turn。

## 结果语义（`HookResult`）

- `blockingError`：阻断错误，注入用户消息并终止该动作（`PreToolUse` 跳过工具、
  `PermissionRequest` 拒绝调用、`Stop` 注入修正消息）
- `preventContinuation`：阻止 query 循环继续
- `message` / `output`：注入上下文供后续 LLM 读取

`command`/`prompt` 输出若为首字符 `{` 的 JSON，解析 `message` / `blockingError` /
`preventContinuation`。

## 配置（`app_config.h` `keys::HOOKS_*`）

| key | 默认 | 说明 |
| --- | --- | --- |
| `hooks.enabled` | `true` | 总开关；关闭则不构建 HookManager（空 manager 短路） |
| `hooks.definitions` | `[]` | JSON 字符串数组，元素 match `HookDefinition` |
| `hooks.timeout_ms` | `30000` | 全局默认超时（单 hook `timeout_ms` 覆盖） |

`HookDefinition` JSON（字段均可选）：`event` / `type` / `match` / `command` /
`url` / `headers` / `prompt` / `model` / `timeout_ms` / `statusMessage` / `once` /
`async` / `asyncRewake`。`match` 为 permission-rule 语法（`"Bash(rm -rf *)"`、
`"Bash \|\| Read"`）。

## 装配与线程安全

统一入口 `make_hook_manager(cfg, provider, bus)`（`hook_manager.cpp`），供
`QueryEngine`（per-query 循环级）与 `ChatSession`（会话级）复用，读取配置、
注入 provider/event_bus。

- **线程安全**：`dispatch` 在互斥锁内对匹配条目做快照 + 乐观占用 `once`，锁外
  顺序执行 `run_hook`。长耗时 hook（LLM/子进程）不会串行阻塞其它触发线程。
- **热路径零开销**：`empty()`/`size()` 不加锁，依赖运行期不变量（注册仅在首个
  dispatch 之前完成）。
- **空指针优雅降级**：所有宿主调用点均以 `hooks && !hooks->empty()` 守卫。

## 事件接入点

| 宿主 | 事件 |
| --- | --- |
| `ReActLoop` | `PreToolUse` / `PostToolUse` / `Stop` |
| `ChatSession` | `SessionStart`（`configure_session_store`）/ `SessionEnd`（析构） |
| `AgentTool` | `SubagentStart` / `SubagentStop`（子级折叠 Stop） |
| `Executor::check_permissions` | `PermissionRequest` |

## 进度可视化（M-2）

`HookManager::run_hook` 经 `IEventBus.publish_async` 发布 `HookProgressEvent`
（`agent_events.h`）：`phase ∈ {start, done, failed}`，`hook_id` 关联同一
hook 的 start/done 两拍，`hook_label` 为展示标签（`[c] 命令` / `[http] url` /
`[prompt] 摘要`）。

TUI 侧：`EventBridge.subscribe_typed<HookProgressEvent>` → `ActionHookProgress`
→ `ViewModel::hook_progress`（按 `hook_id` 合并，FIFO 上限 8 行）→ `App::
build_hook_progress_elem()` 在输入区上方渲染多行进度条（进行中 ● 蓝 / 完成 ✓ 绿 /
失败 ✕ 红）。

## 实现状态

- [x] 8 事件 + 4 类型骨架、matcher、配置加载、dispatch 线程安全
- [x] 进度可视化事件 + TUI 进度条
- [x] `command` / `http` / `prompt` 执行器
- [ ] `agent` 类型执行器（`run_agent` 待实现）
- [ ] frontmatter 对象式 hooks 注册（`Scanner` 式生命周期，会话级清理）

## 测试

`tests/unit/agent/core/test_hook_manager.cpp`：事件枚举 round-trip、matcher 各
语法、command 执行、`once` 语义、`from_json`、prompt 判定（阻断/放行）、M-2
进度事件（`hook_id` 关联 + `hook_label`）。