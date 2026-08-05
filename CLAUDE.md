# CLAUDE.md

Behavioral guidelines to reduce common LLM coding mistakes. Merge with project-specific instructions as needed.

**Tradeoff:** These guidelines bias toward caution over speed. For trivial tasks, use judgment.

## Plan 模式

编写计划时，优先写入项目中的 `.claude/plan/` 目录：

```
.claude/plan/
└── <计划文件>.md    # 计划文件
```

- 使用项目内的 plan 目录，便于版本控制和团队共享
- **查找计划文件时，优先查找 `.claude/plan/` 目录**
- 避免使用 `C:\Users\xxx\.claude\plans\` 路径

---

**Output Format**: Reply in Chinese (中文), end with "喵！"

## 1. Think Before Coding

**Don't assume. Don't hide confusion. Surface tradeoffs.**

Before implementing:
- State your assumptions explicitly. If uncertain, ask.
- If multiple interpretations exist, present them - don't pick silently.
- If a simpler approach exists, say so. Push back when warranted.
- If something is unclear, stop. Name what's confusing. Ask.

## 2. Simplicity First

**Minimum code that solves the problem. Nothing speculative.**

- No features beyond what was asked.
- No abstractions for single-use code.
- No "flexibility" or "configurability" that wasn't requested.
- No error handling for impossible scenarios.
- If you write 200 lines and it could be 50, rewrite it.

Ask yourself: "Would a senior engineer say this is overcomplicated?" If yes, simplify.

## 3. Surgical Changes

**Touch only what you must. Clean up only your own mess.**

When editing existing code:
- Don't "improve" adjacent code, comments, or formatting.
- Don't refactor things that aren't broken.
- Match existing style, even if you'd do it differently.
- If you notice unrelated dead code, mention it - don't delete it.

When your changes create orphans:
- Remove imports/variables/functions that YOUR changes made unused.
- Don't remove pre-existing dead code unless asked.

The test: Every changed line should trace directly to the user's request.

## 4. Goal-Driven Execution

**Define success criteria. Loop until verified.**

Transform tasks into verifiable goals:
- "Add validation" → "Write tests for invalid inputs, then make them pass"
- "Fix the bug" → "Write a test that reproduces it, then make it pass"
- "Refactor X" → "Ensure tests pass before and after"

For multi-step tasks, state a brief plan:
```
1. [Step] → verify: [check]
2. [Step] → verify: [check]
3. [Step] → verify: [check]
```

Strong success criteria let you loop independently. Weak criteria ("make it work") require constant clarification.

---

## 项目结构约定（workx 专属）

### src/ 分层与单向依赖

| 层 | 目标 | 职责 | 依赖 |
|---|---|---|---|
| `core` | `workx_core`（可安装） | 基础设施：事件总线（EventBus）、配置、任务管理、Result、文件索引、工具注册 | nlohmann_json、liblogger |
| `agent` | `workx_agent`（可安装） | **Agent Harness**：ReAct 循环、ChatSession、LLM 后端接口、工具系统、上下文压缩、会话持久化 | core |
| `tui` | `workx_tui`（内部） | 终端 UI：渲染、输入、小组件、tree-sitter 语法高亮 | agent |
| `app` | `workx_app`（内部） | 应用组装：工厂函数、CLI 解析、内置命令、main 入口 | tui |

**单向依赖**：`core ← agent ← tui ← app`，禁止反向 include（`agent→{tui,app,example}`、`core→{agent,tui,app,example}`），由 `tests/unit/agent/test_layer_boundary.cpp` 编译期校验。新增 include 前先确认方向。

### 反向依赖设计（历史 6 处已消除，新增代码遵守）

宿主能力与 UI 专属内容不得上渗到 agent/core，三种消除机制：

- **宿主能力 → 回调注入**：agent 只声明回调（如 `ToolContext::on_file_system_changed`、`ChatSession::set_file_index_invalidator`），宿主（tui/app/外部工程）自己接线
- **共享配置/类型 → 下沉所属层**：`app_config` 迁至 `agent/config/`（内容本在 `namespace agent`）、`FileIndex` 下移 `core/utils/`（tui 也在用）
- **UI 专属校验 → 纯函数解耦**：agent 侧 `ask_user_config.h` 纯函数校验，tui 侧渲染自解析、事件结果在宿主侧转换（`ChoiceResult → agent::AskUserResult`）

事件协议保持宿主无关：`core/events/*` 只承载数据（如 `AskUserResult`），promise 异步协议是工具线程→宿主主线程的正确 marshalling，宿主不订阅则工具走超时分支。

### Agent Harness 目标

- `workx_core` + `workx_agent` 通过 install/EXPORT 对外发布（包名 `workx`，目标 `workx::core` / `workx::agent`）；`workx_tui`/`workx_app` 不安装
- 外部工程三种消费模式均有验证（`tests/consumer/`：find_package / add_subdirectory / 仓库内）：注入 fake `ICompletionProvider` + 订阅 EventBus 即可驱动 ReAct 循环，**零 TUI 依赖**
- 公共 API 面 = `WORKX_PUBLIC_HEADERS` 白名单（`src/CMakeLists.txt`，未列入即私有）；`WORKX_API` 导出宏（`src/core/export.h`）为未来 DLL/插件化预留
- 改动公共头 = 改动白名单闭包（`tests/consumer` 编译验证）；改动分层边界需过 `[layer_boundary]` 单测

---

**These guidelines are working if:** fewer unnecessary changes in diffs, fewer rewrites due to overcomplication, and clarifying questions come before implementation rather than after mistakes.
