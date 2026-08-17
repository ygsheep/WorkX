# FTXUI 实验 TUI — 审查与长期可维护性规划

> 状态：已确认（2026-08-18）
> 审查对象：`src/ftxtui/`（注意目录名为 **ftxtui**，FTXUI 实验 TUI，复用 agent + core）
> 审查基线：`docs/plans/2026-08-17-ftxui-tui-design.md`（设计契约）
> 审查立场：反向举证（假设有错，找证据）；沿用 `.claude/skills/cpp-code-review` 流程

---

## 0. 决策记录

| 决策点 | 结论 | 说明 |
| --- | --- | --- |
| 审查基线 | 设计文档 §3/§5/§8 为契约 | 评估实现与设计的偏离度 |
| 长期目标 | 从「实验」提升为生产级 TUI | 有测试、单职责、零复制复用 agent harness，具备替代 `src/tui` 的资格 |
| 执行顺序 | B2 统一命令 + A2 建立测试 起步 | 先消除「双实现漂移」与「无回归网」两大长期风险 |
| 工作线拆分 | UI 线路 + agent harness 接入线路 | 并行推进，各自独立验收 |

---

## 1. 当前状态核查（审查结果）

### 1.1 架构亮点（保留）

- `EventBridge → ActionQueue → ViewModel(单线程)` 桥接模型与设计 §3 完全一致：后台只入队，不触碰 UI 状态。为核心正确决策。

### 1.2 🔴 High 级

| # | 发现 | 证据 | 影响 / 建议 |
|---|---|---|---|
| H1 | 无任何单测 | 全模块 0 测试文件；项目其他模块 5 个测试目标、661 用例 | markdown 渲染、VM 15 个 action、composer UTF-8 光标全凭手感。长期可维护硬伤 |
| H2 | 命令路由重复分裂 | `app.cpp::send_input` 本地硬编码 `/exit /model /help /clear /resume /rename`；`main.cpp::setup_input_pipeline` 走 InputProcessor 另有一套 `/resume /rename` | 同一命令两条路径，改一边漏一边。应统一到单一 CommandRegistry |
| H3 | 会话装配复制而非复用 | `main.cpp::create_min_session` + `register_min_tools` 复刻 `workx_app::create_session`，仅注册 7 个工具 | 与真实 app 工具集漂移；新增工具两边都要改 |
| H4 | 硬编码绝对路径 + 平台 API | `app.cpp::log_run` 写死 `D:\develop\Workspace\workx\codex_run.log`，且用 `localtime_s`（未 `#ifdef _WIN32` 保护） | 违反项目 `~/.workx/logs` 约定；非 Windows/CI 构建即错 |

### 1.3 🟡 Medium 级

| # | 发现 | 证据 |
|---|---|---|
| M1 | 空转重绘线程 | `run()` 每 80ms 无条件 PostEvent，IDLE 也烧 CPU；应仅 busy 时驱动 |
| M2 | 单例渗透到业务侧 | main 用 `EventBus::instance() / TaskManager::instance() / ConfigManager::instance()`；仅 main 边界可接受 |
| M3 | `sidebar_rule()` 空实现 stub | `sidebar.cpp` 返回 `emptyElement`，却被调用两次 |
| M4 | AskUser 能力降级 | `handle_ask_user` 只用 `questions[0]`，`cancel_flag` 未消费；设计 §5 要求多问题 + 自定义输入 |
| M5 | session_id 硬编码 | 回声事件固定 `"default"`，mismatch 审计日志真实会话 ID 约束 |

### 1.4 🔵 Low 级

- L1 魔法数散落 app.cpp（`kComposerHeight=3`、`kSidebarCollapseWidth=100`）未入 theme。
- L2 用户文案散落各 .cpp，未集中可翻译层。
- L3 布局估算 `layout_rows` 与渲染 `build_transcript` 双份对齐，需常驻同步（易漂移）。

### 1.5 审查结论

⚠️ **结构正确但未达「可长期维护」门槛**：缺测试、命令/装配两处重复、路径不跨平台。

---

## 2. 长期目标

把 `src/ftxtui` 从「实验」提升为**有测试、单职责、零复制复用 agent harness** 的生产级 TUI，
具备替代 `src/tui` 的候选资格。两条工作线并行推进。

---

## 3. Part A — UI 线路

| 阶段 | 目标 | 关键动作 |
|---|---|---|
| A1 主题收敛 | 消灭魔法数与 stub | `kComposerHeight / SidebarCollapseWidth / 状态行高 / 侧栏宽` 下沉到 theme；补全或删除 `sidebar_rule()` |
| A2 渲染可测 | UI 逻辑进单测 | `markdown_to_elements`、`message_node`、`view_model.apply`（15 个 action 全覆盖）、`composer` UTF-8 光标 golden 测试 |
| A3 单一布局源 | 消除滚动手感漂移 | 把 `layout_rows/approx_height` 与 `build_transcript` 收敛为同一份布局描述（单一数据源） |
| A4 性能 | IDLE 零耗 | 移除无条件 80ms 重绘；仅 busy 或数据变更时驱动帧 |
| A5 可访问/中文化 | 体验层 | 文本集中到独立 strings 层；ASCII 降级（Nerd Font 不可用时）；`Ctrl+O`/滚轮已支持，补键盘导航单测 |

---

## 4. Part B — agent harness 接入

| 阶段 | 目标 | 关键动作 |
|---|---|---|
| B1 复用装配 | 删除复制 | 抽共享 `create_session`/工具装配到可复用库，`src/app` 与 ftxtui 双端调用同一实现（工具集同步） |
| B2 统一命令 | 单一执行路径 | App 删掉私有 `/resume /rename /model` 实现，统一走 `agent::command::CommandRegistry`；ftxtui 命令面板改为同一注册表薄视图，移除双注册表 |
| B3 桥接补全 | 对齐设计 §5 | 订阅 `CacheDiagnostics / CompactionPaused / SubAgent / 权限内联`；AskUser 支持 `questions[] + allow_custom_input + cancel_flag`；回声事件注入真实 session_id |
| B4 生命周期安全 | 跨平台 + 退订 | 修复 `log_run` 路径（→ `~/.workx/logs`）与 `localtime_s` 平台保护；`EventBridge.stop()` 真正按 token 退订，不依赖外部 `bus.clear()` 兜底 |
| B5 接缝测试 | 稳 harness 边界 | bridge event→action 映射、超时/取消路径、命令分发做单测；`--mock` 提为 CI 冒烟用例 |

---

## 5. 执行建议

从 **B2 统一命令 + A2 建立测试** 起步——最先消除「双实现漂移」与「无回归网」两大长期风险；
随后 B1 复用装配、B4 生命周期安全（跨平台）；最后 A4 性能与 A5 体验作为打磨项。

---

## 6. 后续验收清单（Checklist）

- [ ] A1 全部魔法数迁入 theme；`sidebar_rule()` 删除或实现
- [ ] A2 建立 ftxtui 单测目标；markdown / VM action / composer 光标均有覆盖
- [ ] A3 `layout_rows` 与 `build_transcript` 合并为单一布局源
- [ ] A4 IDLE 时无重绘线程占用
- [ ] A5 用户文案集中；Nerd Font 有 ASCII 降级
- [ ] B1 `src/app` 与 ftxtui 共享受会话装配，工具集一致
- [ ] B2 单一命令执行路径；命令面板消费统一注册表；移除双注册表
- [ ] B3 桥接补齐设计 §5 事件；AskUser 多问题 + cancel_flag；真实 session_id
- [ ] B4 `log_run` 路径平台无关；`EventBridge.stop()` 真正退订
- [ ] B5 bridge 映射 / 超时取消 / 命令分发单测通过；`--mock` 进 CI 冒烟