# 通用 Hook 事件系统 — 实现方案

> Issue #50 · milestone 0.7.x Hooks + 压缩升级 · P1
> 本文档基于对现有代码库的只读调研，提出可落地的实现方案。

***

## 1. 需求回顾

参考 cc（claude-code）的 hook 模型，落地一个通用事件钩子系统：

* **8 个事件**：`PreToolUse` / `PostToolUse` / `SessionStart` / `SessionEnd` / `Stop` / `SubagentStart` / `SubagentStop` / `PermissionRequest`

* **4 种类型**：`command`（shell）/ `prompt`（LLM 评估）/ `agent`（agentic verifier）/ `http`（POST）

* **能力**：`blockingError`（注入用户消息阻断）、`preventContinuation`（阻止 query 循环继续）

* **注册**：frontmatter hooks（agent/skill 级生命周期）+ 配置文件注册 + `if` permission-rule 条件匹配

衡量标准：可配置 PreToolUse 拦截危险操作、Stop hook 可阻断并注入修正、进度可视化、单元测试覆盖各类型/优先级。

***

## 2. 现状分析

| 现有能力                                         | 位置                                                      | 是否可复用                              |
| -------------------------------------------- | ------------------------------------------------------- | ---------------------------------- |
| Skill PreActivate hooks（仅执行 shell，"激活前"单一场景） | `src/agent/skill/inclaude/hooks.h` + `source/hooks.cpp` | 部分复用（`command` 类型执行器直接借鉴）          |
| 跨平台子进程封装（超时/输出捕获/取消/截断）                      | `src/core/process/subprocess.h` `exec()`                | **直接复用**（command 类型）               |
| HTTP 客户端（curl、SSRF 防护）                       | `src/api/remote/http_client.cpp`                        | **直接复用**（http 类型）                  |
| 事件总线（类型化订阅/发布 + 异步 drain）                    | `src/core/events/event_bus.h` `IEventBus`               | **直接复用**（事件通知/进度可视化）               |
| LLM 推理 provider                              | `ICompletionProvider`                                   | **直接复用**（prompt/agent 类型）          |
| 配置管理（schema 注册 + env 覆盖 + JSON 落盘）           | `src/core/config/` `app_config.cpp`                     | **直接复用**（hook 配置注册）                |
| 权限流程（`check_permissions` + AskUser 通道）       | `src/agent/tool/permission_ask.cpp`                     | 注入点位（PermissionRequest）            |
| 权限规则匹配                                       | `src/agent/tool/path_matcher.cpp` 等                     | **参考**（if 条件可仿 permission rule 语法） |

**缺口**：目前没有任何通用事件钩子的注册/调度/执行器抽象，所有工具执行/停止/子代理路径都是硬编码流程。

***

## 3. 总体架构

新增一个独立的 hook 调度核心（宿主无关、可单测），向 agent harness 暴露统一的调用点 API。

```
               ┌─────────────────────────────────────────────┐
               │              HookManager (agent::hook)       │
               │  · 事件 HookManager::dispatch(Event, Ctx)    │
               │  · 类型 HookExecutor (command/prompt/agent/http) │
               │  · 条件 ifMatcher (permission-rule)          │
               │  · 注册 registry（frontmatter + config）     │
               └─────────────────────────────────────────────┘
                                   ▲
                 dispatch(PreToolUse/PostToolUse/Stop/...)
                                   │
   ┌─────────────┬─────────────┬───┴──────┬─────────────┬──────────────┐
ReActLoop     ToolExecutor  ChatSession   AgentTool    permission_ask
(react_loop)  (check_perms) (session)   (subagent)     (ask_user)
 工具前/后/停止   权限请求       会话启/停     子代理启/停     权限确认
```

### 3.1 目录规划（新增）

```
src/agent/hook/                     # 新命名空间 agent::hook
├── hook_event.h                    # HookEvent 枚举 + HookEventData 上下文负载
├── hook_executor.h/.cpp            # command/prompt/agent/http 四种执行器
├── hook_manager.h/.cpp             # 注册表 + 调度 + 结果聚合（浏览器）
└── hook_match.h/.cpp               # if 条件 permission-rule 匹配（编译一次 matcher）
```

在 `src/agent/CMakeLists.txt` 的 `workx_agent` target 下新增 `hook/` 源文件（约 4\~5 个 .cpp）。

### 3.2 核心数据结构

```cpp
// hook_event.h
enum class HookEvent {
    PreToolUse, PostToolUse, SessionStart, SessionEnd,
    Stop, SubagentStart, SubagentStop, PermissionRequest
};

// 每种事件携带的上下文负载（联合/按事件取用）
struct HookContext {
    std::string session_id;     // 当前会话
    std::string cwd;            // 工作目录
    std::string request_id;

    // PreToolUse / PostToolUse / PermissionRequest
    std::string tool_name;      // 工具名（Bash/Read/Write/...）
    nlohmann::json tool_input;  // 工具参数
    std::string tool_result;    // PostToolUse：工具原始返回
    bool tool_error{false};     // PostToolUse：是否出错

    // Stop / SubagentStop
    std::string final_answer;   // Agent 收尾答复
    std::string stop_reason;    // interrupted / error / completed / at_limit

    // SubagentStart / SubagentStop
    std::string subagent_id;    // 子代理 task_id
    std::string subagent_prompt;
};

// 执行器返回（Promise 语义），同 cc
struct HookResult {
    std::string message;            // 注入用户可见信息
    std::optional<std::string> blockingError;  // 阻断错误注入用户消息
    bool preventContinuation{false};           // 阻止 query 循环继续
    std::string stopReason;                    // 附加 stop reason
};
```

### 3.3 Hook 注册结构

```cpp
struct HookDefinition {
    std::string event;        // "PreToolUse" / "Stop" / ...
    std::string type;         // "command" / "prompt" / "agent" / "http"
    std::string match;        // if 条件，permission-rule 语法，如 "Bash(git *)"
    // 类型相关载荷
    std::string command;      // command：shell 命令
    std::string url;          // http：POST URL
    nlohmann::json headers;   // http：自定义请求头
    std::vector<std::string> allowedEnvVars; // 允许透传的 env
    std::string prompt;       // prompt/agent：LLM 提示
    std::string model;        // prompt/agent：指定模型
    std::string agent;        // agent：agentic verifier
    int timeout_ms{30000};    // 超时
    bool statusMessage{false};// 显示状态信息
    bool once{false};         // 只运行一次
    bool async{false};        // 异步（不阻塞）
    bool asyncRewake{false};  // 异步唤醒重置 once
};
```

***

## 4. 8 事件插入点（核心）

基于调研的 ReAct 流水线，逐一定位刀具位置。**推荐统一收敛在** **`ReActLoop`** **+** **`ToolExecutor`** **+** **`ChatSession`** **三处调度**，避免散落。

| 事件                    | 插入位置                                                          | 文件:行（参考）                          | 能拿到的字段                                            |
| --------------------- | ------------------------------------------------------------- | --------------------------------- | ------------------------------------------------- |
| **PreToolUse**        | Action 步骤发布后、异步 `std::async` 执行前 — 单线程、可同步阻断                  | `react_loop.cpp` \~940-964        | tool\_name, tool\_use\_id, tool\_input            |
| **PostToolUse**       | `exec.future.get()` 返回后、写入 history 前                          | `react_loop.cpp` \~1000-1018      | tool\_name, result\_text, is\_error, duration\_ms |
| **PermissionRequest** | `tool->check_permissions()` 前（Bypass/Plan/Default 决策处）        | `executor.h` \~177-186            | tool\_name, input, permission\_mode               |
| **SessionStart**      | 首条 user 消息懒创建 SessionStore 时 / `new_session()`                | `chat_session.cpp`（懒创建处）          | session\_id, cwd, model                           |
| **SessionEnd**        | 会话析构 / 切换旧 session（注意 `switch_session` 不写 session\_end，需语义一致） | `~ChatSession` / `switch_session` | session\_id, 会话统计                                 |
| **Stop**              | 主循环统一收尾处（汇总 was\_interrupted/was\_error/final\_answer）        | `react_loop.cpp` \~1099-1128      | stop\_reason, final\_answer, was\_error           |
| **SubagentStart**     | 子 ReActLoop 构造前 / `loop.run()` 前                              | `agent_tool.cpp` \~99-110         | subagent\_id, subagent\_prompt                    |
| **SubagentStop**      | `loop.run()` 返回后、发布 `SubAgentCompletedEvent` 前                | `agent_tool.cpp` \~139-160        | subagent\_id, final\_answer, was\_error           |

### 4.1 Stop 的阻断语义实现

`blockingError` / `preventContinuation` 需要**同步**作用于当前 turn。由于 HookManager 是纯同步接口，Stop hook 在循环收尾处同步执行，返回值直接写入：

* `blockingError.has_value()` → 注入一条 **user 角色消息** `{role:user, content: blockingError}` 到 `messages`，然后令 `graceful_stop=true`（不再继续），并可标记 `result.was_error=true` 或附加 stop\_reason。

* `preventContinuation=true` → 等同不进入下一轮循环（`graceful_stop=true`）。

* `message` 非空 → 作为附加 assistant 提示，合并进最终答复。

> 注意：Stop hook 语义上"在 Agent 决定停止时"触发。需区分「用户取消」「达上限」「自然完成」三种停止原因，按需决定是否触发。参考 cc 的 `interrupted()` / `continue_session` 语义，建议默认对**所有**停止路径统一触发一次，由 hook 的 `if` 条件决定是否生效。

### 4.2 PreToolUse 的注入/拦截语义

PreToolUse 在 async 执行前同步执行（并行工具场景注意串行化影响——建议先同步跑 hook，再进入并行执行）。若返回 `blockingError` 或 `preventContinuation`：

* 该工具不执行，`tu` 结果替换为错误文本（如 `Hook blocked: ...`），记录为 observation。

* `preventContinuation` 在 Thought 阶段即阻断（参考 cc：PreToolUse 返回 blockingError 后 agent 停止）。

### 4.3 Subagent 内嵌 Stop hook

参考 cc：subagent 中的 Stop hooks 应转为 `SubagentStop`。实现：子 `ReActLoop` 完成时，HookManager 在 **父级作用域** 触发的就是 `SubagentStop`；子级内不再重复触发 `Stop`，避免双重执行。

***

## 5. 四种 Hook 类型执行器

| 类型          | 实现                                                                                                                        | 复用点                                |
| ----------- | ------------------------------------------------------------------------------------------------------------------------- | ---------------------------------- |
| **command** | 复用 `agent::process::exec()`：`cmd.exe /d /s /c`（Win）/ `sh -c`（POSIX），传 cwd/timeout/is\_cancelled，输出截断                      | 直接借鉴现有 `hooks.cpp` 的 shell 拼接与截断逻辑 |
| **http**    | 复用 `src/api/remote/http_client.cpp`（curl + SSRF），POST `url`，带 `headers` + `allowedEnvVars`；放入 hook output 供后续 LLM 读取；超时控制 | 复用现有 client                        |
| **prompt**  | 构造 `CompletionRequest`，调 `ICompletionProvider` 评估 hook.prompt，单次推理；超时用 `timeout_ms`                                       | 复用 provider                        |
| **agent**   | 构造一个受限子 ReActLoop（仅可调用白名单工具），围绕 hook.prompt 做 agentic verifier，产出判定                                                       | 复用 `ReActLoop` + `registry`        |

### 5.1 执行结果聚合

* **同步阻塞类型的 hook（command/http/prompt/agent 默认）**：返回 `HookResult`，含对注入的 `output` 文本（供 prompt/pause 注入消息上下文）。

* **异步（async/asyncRewake）**：不阻塞主线程，用 `EventBus.publish_async` 通知宿主（`HookOutputEvent`），宿主只在 UI 上展示状态。

* 多 type 执行时按注册顺序组成 hook output 文本块，注入下一条 user/assistant 消息之前（prompt/agent 类型更适用）。

***

## 6. 注册机制

### 6.1 来源 A — frontmatter hooks（agent/skill 生命周期）

沿用现有 `SkillFrontmatter::hooks` 的解析管线（`frontmatter.h` / `skill_loader.cpp`），但**扩展语义**：除当前"激活前执行 shell"外，再支持结构化对象形式的 hook 定义（event + type + 配置）注册到 `HookManager`：

```yaml
---
hooks:
  - event: PreToolUse
    type: command
    match: "Bash(rm *)"
    command: "echo not allowed"
    blockingError: "禁止执行 rm"
---
```

`schema 兼容性`：现有 `hooks: [string]`（纯命令，激活前）保持不变；新增对象形式做类型判别（string → 老逻辑，object → 通用事件注册）。Skill 激活时注册、会话结束时 `clearSessionHooks()` 清理（对齐 cc）。

### 6.2 来源 B — 配置文件

在 `app_config.h` 注册新的配置键，schema 类型用 JSON：

```cpp
constexpr const char* HOOKS = "hooks";   // JSON 数组，元素为 HookDefinition
```

`register_config_defaults()` 中注册：

* `hooks`：默认空数组

* `hooks.timeout_ms`：全局 hook 默认超时（默认 30000）

* `hooks.enabled`：总开关（默认 true）

ConfigManager 的 `flatten_json` 支持 dot 访问；HookManager 启动时从 `IConfigManager::get<json>("hooks")` 读取并编译 matcher。

### 6.3 if 条件匹配（`HookMatch` / `ifMatcher`）

* 语法对齐 cc permission-rule：`ToolName(arg pattern)`，模式间 `||`；`*.md`、`git *` 等通配。

* 复用现有 `path_matcher` / glob 能力做 glob→regex 转换。

* **关键**：为每个 hook 的 `if` 字段在注册时**一次性编译成 matcher**（cc 的 `preparePermissionMatcher`），避免每次 dispatch 重新解析。

* 匹配失败 → 跳过该 hook；匹配成功 → 执行。

***

## 7. 进度可视化

* 新增事件类型 `HookStartedEvent` / `HookCompletedEvent` / `HookFailedEvent`（可放 `agent_events.h` 或新建 `hook_events.h`），HookManager 经 `EventBus.publish_async` 发布。

* TUI `EventBridge`（`src/tui/bridge/event_bridge.cpp`）用 `subscribe_typed<T>` 订阅，渲染为 hook 执行卡片（起始/结果/输出）。

* 复用与 Skill 卡片类似的折叠渲染（现有 skill hooks 输出已显示 `[ok]/[fail]` 文本块）。

***

## 8. ReActLoop 配置接线

在 `ReActLoop::Config` 新增字段：

```cpp
struct Config {
    ...
    std::shared_ptr<agent::hook::HookManager> hooks;  // 可空；为空则全部跳过
    int hook_timeout_ms{30000};
};
```

`QueryEngine::make_react_config()` 从 `IConfigManager` 读取 `hooks` / `hooks.timeout_ms`，构造 `HookManager` 注入。**空指针优雅降级**：未配置 hook 时 hook 调度是零开销的 if-null 短路，不影响现有路径。

***

## 9. 实施步骤（建议拆分 PR）

| 步骤     | 内容                                                                       | 验证                           |
| ------ | ------------------------------------------------------------------------ | ---------------------------- |
| **S1** | `hook_event.h` + `HookManager` 骨架（注册表 + dispatch 空实现 + if matcher）       | 单测：注册/匹配/dispatch 排序         |
| **S2** | `command` + `http` 执行器 + PreToolUse/PostToolUse 接入 ReActLoop             | 集成：Bash 前置检查、结果记录            |
| **S3** | `prompt` + `agent` 执行器                                                   | 单测：LLM 评估判定                  |
| **S4** | Stop/SessionStart/SessionEnd 接入 + blockingError/preventContinuation 阻断语义 | 集成：Stop 阻断并注入修正消息            |
| **S5** | SubagentStart/SubagentStop 接入（子 Stop 折叠）                                 | 集成：子代理 hook 仅触发 SubagentStop |
| **S6** | PermissionRequest 接入 executor 权限决策处                                      | 集成：动态授权                      |
| **S7** | frontmatter 对象形式 hooks + 配置文件加载 + 进度事件 + TUI 卡片                          | 端到端验证 + 文档更新                 |

建议 PR 基分支为 `develop`。

***

## 10. 风险与权衡

1. **并行工具执行**：PreToolUse 若在 async 循环内同步跑，会串行化工具执行。权衡：PreToolUse 需在进入并行前批量同步执行（统一先跑所有 tu 的 hook），PostToolUse 可在各 future 完成后独立触发。
2. **Stop hook 的副作用**：`blockingError` 注入 user 消息会落盘（SessionStore），需保证幂等、不污染历史语义。
3. **异步 hook 与 SessionStore**：async hook 输出不落盘，仅 UI 展示，需明确定位避免状态不一致。
4. **prompt/agent 类型成本**：每次 hook 触发 = 一次 LLM 调用，需默认超时 + 显式启用，防拖慢主链路。
5. **`switch_session`** **不写 session\_end**：SessionEnd 钩子语义需与现有持久化约定对齐（不可每次 resume 都清 hook 状态）。

***

## 11. 参考文件清单

| 用途             | 路径                                                                      |
| -------------- | ----------------------------------------------------------------------- |
| 现有 skill hooks | `src/agent/skill/inclaude/hooks.h`、`source/hooks.cpp`                   |
| 跨平台子进程         | `src/core/process/subprocess.h`、`subprocess.cpp`                        |
| HTTP 客户端       | `src/api/remote/http_client.cpp`                                        |
| ReAct 主循环      | `src/agent/core/react_loop.cpp`（\~940/1000/1099）                        |
| 工具执行器          | `src/agent/tool/executor.h`（\~177 check\_permissions）                   |
| 权限确认           | `src/agent/tool/permission_ask.cpp`                                     |
| 会话生命周期         | `src/agent/core/chat_session.cpp`、`chat_session.h`                      |
| 子代理            | `src/agent/tool/AgentTool/agent_tool.cpp`（\~99/139）                     |
| 事件总线           | `src/core/events/i_event_bus.h`、`event_bus.h`、`agent_events.h`          |
| 配置             | `src/agent/config/app_config.h/.cpp`、`src/core/config/config_manager.h` |
| 配置→agent 桥接    | `src/agent/core/query_engine.cpp`                                       |
| TUI 事件桥        | `src/tui/bridge/event_bridge.cpp`                                       |
| 构建             | `src/agent/CMakeLists.txt`                                              |

