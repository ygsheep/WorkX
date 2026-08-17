# 实验 TUI 设计 — `workx-ftx`（FTXUI 双栏渲染器）

> 状态：已确认（2026-08-17）
> 目标：在不触碰 `src/tui` 的前提下，用 FTXUI 重做渲染+输入层，复用现有 agent + core + EventBus。

## 0. 决策记录

| 决策点 | 结论 | 说明 |
| --- | --- | --- |
| 与现有 agent 的关系 | 只换渲染+输入层 | 复用 `EventBus + ChatSession/ReActLoop`，不重写 harness |
| 目录 | `src/ftxtui/` | 与 `src/tui` 平级，零链接/零依赖 |
| 布局模型 | opencode 双栏 | 左对话流 + 右可折叠侧栏，窄屏自动折叠 |
| 里程碑 | 最小可用 + 交互面板 | 真实对话端到端跑通 + 命令/模型/技能面板 |
| 桥接模型 | ActionQueue 事件桥 + 流式节流 | 后台只入队 action，UI 线程每帧 drain；token 节流重绘 |
| 渲染复用 | 复刻 `markdown_renderer` 思路 | 解析结构不变，输出从 ANSI 字符串改为 `ftxui::Element` |

## 1. 目标与硬约束

- 不改动 `src/tui` 任何代码；不 `#include`、不链接 `workx_tui` / `src/tui/*`。
- 复用：`workx_core`（EventBus/子进程/任务）、`workx_agent`（`ChatSession`、`ReActLoop`、`InputProcessor`、`CommandRegistry`、会话存储）、`IBackendAdmin`。
- 可执行目标 `workx-ftx`，与 `workx` 并存；默认关闭，由 `WORKX_BUILD_FTX_TUI` 开关控制。
- 基于 **FTXUI 即时渲染**：每帧重建组件树，ftxui 内部 diff。模型由此派生（见 §3）。

## 2. 目录结构

```
src/ftxtui/
  main.cpp                  // 装配：create_session + 组件树 + 事件接线
  app.cpp / app.hpp         // 顶层 ftxui::ScreenInteractive + 组件树装配
  vm/
    view_model.*            // 文档模型（UI 线程独有）：消息列表 / 侧栏 / 输入 / 面板
    message_node.*          // user / assistant / tool_call / observation / thinking 节点
  bridge/
    action.*                // 枚举式 TuiAction（append_message / token_delta / tool / done / sidebar ...）
    action_queue.*          // 并发安全队列（后台写 / UI 读）
    event_bridge.*          // EventBus 订阅 → ActionQueue
  render/                   // 自有渲染适配（不复用 src/tui/render）
    markdown_to_elements.*  // 复刻 markdown_renderer 解析结构 → ftxui::Element
    tool_call_view.*
  widgets/
    sidebar.*               // 右侧上下文/成本/标题面板（可折叠）
    composer.*              // 底部多行输入 (ftxui::Input)
    status_line.*           // composer 上方状态行（模型/权限/模式）
    command_palette.*       // 命令面板（ftxui::Menu，页内过滤）
    model_selector.*        // 模型切换模态
    skill_panel.*           // 磁盘 skills 列表
  CMakeLists.txt
```

`EventBridge` 与 `ViewModel` 之间**只通过 `TuiAction` 交互**，保证后台线程不触碰 UI 状态。

## 3. 架构与线程模型（核心）

ftxui 是立即模式渲染器，据此采用「事件 → 不可变 action → 每帧应用 → 重建」模型：

```
后台（EventBus 回调）              UI 线程（ScreenInteractive::Run）
  StreamTokenEvent ─┐
  ToolCallEvent     ├─► ActionQueue(TuiAction) ─► 每帧 drain ─► 修改 ViewModel ─► 重建组件树
  TaskDoneEvent     ┘       (并发队列)                     │        ▲
                                                       build_markdown_elements
  UserInputEvent ◄──────────── 输入事件 ←───────────────────┘
   AskUser promise  ◄───────── 模态面板回填 ─────────────────┘
```

- **ActionQueue 桥**：事件回调（可能位于后台/事件泵线程）只 `push` 一个轻量 `TuiAction`，不持锁碰 UI。UI 线程每帧 `drain()` 全部 action 并应用到 ViewModel，再触发一次 `RequestAnimationFrame()`。
- **流式节流**：`StreamTokenEvent` 不逐 token 触发重建——delta 仅追加到当前 `StreamingMessage` 节点的原始缓冲，按 ~15–30fps（或文本长度阈值）节流刷新对应消息的 markdown Element。与现有 `StreamingBuffer` 语义等价，但状态归 ViewModel。
- **单线程 ViewModel**：所有 UI 状态只被 UI 线程读写；后台只入队。消灭当前 `src/tui` 里"事件泵线程 + 锁 + overlay 快照"的复杂并发面。
- **关闭语义**：退出按 退订 → `TaskManager::waitForAll` → 关 SessionStore → reset session → screen ExitLoop 的顺序清理。

## 4. 布局（opencode 双栏）

- **左栏（主，聊天流）**：`ftxui::vscroll`。
  - 用户消息（角色标签）
  - 助手 turn：**可折叠工具调用块**（运行中带 spinner）+ 流式正文（markdown）
  - 思考内容：默认折叠、dim 显示，`Ctrl+O` 展开
  - 工具结果：可展开，含 markdown / 代码块
- **右栏（侧栏，可折叠）**：会话标题、上下文使用 progress bar、token 数、成本 `$`、权限模式标签、Agent 名。**宽度低于阈值自动折叠**；横宽时展开——平衡"信息密度"与用户偏好"最大化内容区"。
- **顶栏**：极简一行（项目 / 分支 / 模型），压到最矮。
- **底部 composer**：`ftxui::Input` 多行（Shift+Enter 换行）+ 其上单行 status line。

## 5. 数据流 / 事件映射

复用现有输入链：composer 提交 → `EventBus.publish(UserInputEvent)` → 现有 `InputProcessor → 命令/展开` → `ChatSession::send_message`。渲染侧订阅后映射为 action：

| 事件 | → TuiAction |
| --- | --- |
| `StreamTokenEvent` | `TokenDelta`（写入当前流式节点缓冲）|
| `StepDoneEvent` | `StepDone`（轻量收尾，提交当前消息节点）|
| `StreamDoneEvent` | `TurnDone`（提交 + 状态回 IDLE + token 统计）|
| `StreamErrorEvent` | `PushError` |
| `ToolCallEvent` | `BeginTool`（创建可折叠调用块）|
| `ToolResultEvent` | `EndTool`（写入结果，调用块可展开）|
| `AgentDoneEvent` | `AgentDone`（汇总工具次数/耗时）|
| `CacheDiagnosticsEvent` / `CompactionPausedEvent` | 侧栏/状态栏提示 |
| `SubAgentProgressEvent` / `SubAgentCompletedEvent` | 子任务进度块 |
| `AskUserRequestEvent` | 模态面板，`result_promise->set_value` 回填 |
| `EnterPlanModeEvent` / `ExitPlanModeEvent` | 权限模式标签更新 |

`AskUserRequestEvent` 携带 `promise`/`cancel_flag`，回填与现有 ChoicePanel 语义一致（不持锁）。

## 6. 渲染适配（复刻 markdown_renderer 思路 → Element）

- **解析结构原样迁移**：`render_inline`（粗体/斜体/删除线/行内码/转义）、`TableBuffer`（表格流式状态机）、`render_markdown_block`（代码块/标题/列表/分隔线/行内分派）的逻辑与命名保持一致。
- **输出层替换**：不再输出 ANSI 字符串，而是逐块构建 `ftxui::Element`——`text` + `color`/`bold` 装饰；表格用 `ftxui::GridBox`/`Table`；代码块用 `border` 包裹的等宽区块。
- **代码高亮**：里程碑内代码块先以**边框块+整体无逐 token 高亮**呈现（保持零耦合）；tree-sitter 逐 token 高亮列为后续增强（复用 `vendor/tree-sitter` grammar，自研薄适配器）。
- **流式重渲染**：每个消息节点缓存"原始 markdown 文本 + 当前宽度"，节流时仅对该节点重建 Elements，其余节点复用，控制重建成本。

## 7. 交互面板与快捷键

- 命令面板：`/` 或 `Ctrl+P` → `ftxui::Menu`（注入 `CommandRegistry` 命令，页内过滤，不翻页）。
- 模型切换：模态 `Menu`（走 `IBackendAdmin::list_models`）。
- 技能面板：磁盘 skills 列表。
- 快捷键（对齐现有交互）：`Enter` 提交、`Shift+Enter` 换行、`Ctrl+C` 中断、`Esc` 关模态、`Tab` 补全、`Shift+Tab` 权限循环（Default→Plan→Bypass）、`Ctrl+O` 思考展开、`Ctrl+P` 命令面板、鼠标可选（opencode 风格）。

## 8. 里程碑范围

MVP（本实验交付）：
- 真实对话端到端：流式渲染 + 工具调用/结果（可折叠）+ 多行输入 + 会话恢复。
- opencode 双栏布局（侧栏可折叠）。
- 命令面板 / 模型切换 / 技能面板。
- 顶栏 + 底部 composer + status line。

不含（后续）：per-token 代码语法高亮、权限请求内联对话框、diff/文件视图、主题系统。

## 9. 构建

- FTXUI 未在 `vcpkg.json`：以 `FetchContent` 拉取（`add_subdirectory` 模式），不进 manifest。
- 顶层新增 `option(WORKX_BUILD_FTX_TUI OFF)`，仅 `src/ftxtui/` 编译该 target；不改变现有 targets/依赖。
- `src/ftxtui/CMakeLists.txt` 链接 `workx_agent workx_core`（复用）与 `ftxui::screen ftxui::dom ftxui::component`。

## 10. 风险与对策

| 风险 | 对策 |
| --- | --- |
| 立即模式 + 高频 token 事件导致重建风暴 | 流式节流 + 仅重建流式节点（§6）|
| 后台事件越界碰 UI 状态 | 强制只走 ActionQueue、ViewModel 单线程（§3）|
| markdown→Element 覆盖不全 | 先覆盖 MVP 常用语法，其余降级为纯文本段落 |
| 双栏在窄屏挤压内容 | 侧栏按宽度阈值自动折叠（§4）|
| FTXUI 与现有 raw-mode 冲突 | `workx-ftx` 用独立可执行与独立屏幕生命周期，互不影响 |