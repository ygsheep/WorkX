# 侧边栏 Tab 化 + 文件 Review 查看器规划

> 状态：P1/P2/P3/P4/P5 已完成（2026-08-20）
> 范围：`src/ftxtui`（本计划仅规划，未改代码）
> 基线：`docs/plans/2026-08-17-ftxui-tui-design.md`、`docs/plans/2026-08-18-ftxui-maintainability-plan.md`
> 前置探讨：`/edit` 内嵌 nvim（方案 B）与 `/view` 只读查看器（方案 A）——本计划聚焦 `/view` 的 tab 化落地

---

## 0. 决策记录

| 决策点 | 结论 | 说明 |
| --- | --- | --- |
| Tab 集 | **任务调度 \| 变更记录 \| 文件** 三 tab | 任务调度为常驻默认 tab（无关闭）；变更记录、文件为可开合 tab（带 ✕ 关闭按钮） |
| 默认 tab | 任务调度 | 启动即任务调度视图 |
| 任务调度 tab | **可交互** | 子 Agent 列表可聚焦选中，Enter 跳转转录区对应消息 |
| 文件 tab | **只读 + 行号 + 虚拟化滚动 + 内联 diff 高亮** | 纯内容视图（无上下两区），保留新增/修改行标记，不提供编辑 |
| 变更记录 tab | **内联 diff 视图** | git diff 风格 hunk 视图（+ 新增 / 高亮修改行 + 目的注释），选中修改点跳转文件 tab 对应行 |
| diff 来源 | 会话内 Edit/Write 工具调用 | 修改目的取自该步 reasoning；git diff 列为可选增强 |
| 文件读取 | UI 线程同步读 + 大小上限 | ≤ 2MB，超限截断并提示 |
| 删除行展示 | **仅高亮新增/修改行** | 不渲染 `-` 删除行；删除内容以修改点摘要（old→new）呈现 |
| 修改目的粒度 | **可展开多行** | 默认一行注释，按 `e` 展开完整 reasoning |

---

## 1. 目标与范围

### 1.1 目标

1. 侧边栏升级为顶部 tab 栏 + 内容区的容器，默认「任务调度」。
2. 「任务调度」tab 聚合当前 Agent 活动（busy / 当前工具 / 步骤 / 子 Agent / 后台任务 / 会话统计），且子 Agent 条目可交互跳转转录。
3. 「文件」tab 提供 `/view <file>` 只读查看：行号、虚拟化滚动、内联 diff 高亮（新增/修改行标记）。
4. 「变更记录」tab 提供会话内全部修改的 Review：git diff 风格 hunk + 每处修改目的，选中跳转文件 tab 对应行。

### 1.2 非目标（本期不做）

- 文件编辑（走 `/edit` 内嵌 nvim，方案 B 另行规划）。
- git diff 对比（列为可选增强）。
- 文件树浏览器（仅 `/view <file>` + `@` 文件索引选择器）。

---

## 2. 侧边栏 Tab 化设计

### 2.1 布局

```
┌─────────────────────────────┐
│ 任务调度 │ 变更记录 ✕ │ 文件 ✕ │  ← tab 栏（自绘，非纯 Menu）
├─────────────────────────────┤
│  … 当前 tab 内容区 …        │
└─────────────────────────────┘
```

- 侧栏宽度/折叠阈值沿用 `kSidebarWidth=30`、`kSidebarCollapseWidth=100`。
- tab 栏**自绘**（`hbox` 的 tab 块，非纯 `Menu`，因为需要每 tab 独立的 ✕ 关闭按钮）：
  - 每个 tab = `Renderer`（文字 + 可选 `✕`），选中项 `theme::T::Accent` 背景 + 米白，未选中 `TextDim`。
  - `CatchEvent` 处理鼠标点击：点 tab 文字 → 切换；点 `✕` → 关闭该 tab（若可关闭）。
  - 键盘：`Esc` 关闭当前可关闭 tab（等价 ✕）。
- **可开合规则**：
  - 「任务调度」常驻，无 ✕，不可关闭。
  - 「变更记录」在会话出现首个 FileChange 时自动出现；关闭后清空选中态，返回任务调度。
  - 「文件」由 `/view` 打开；关闭后返回任务调度。
  - 关闭后 tab 从栏上消失；重新打开（/view 或新修改）时按固定顺序插回：变更记录在左、文件在右。
- 窄屏折叠逻辑不变（整体隐藏）。

### 2.2 状态模型（`vm/view_model.h` 扩展）

```cpp
enum class SidebarTab { kTasks = 0, kFiles, kChanges, kCount };

struct SubAgentLite {          // 子 Agent 聚合条目
    std::string task_id;
    std::string status;        // "running" / "done" / "failed"
    std::string current_step;  // 最近一步类型/摘要
    int step_number = 0;
    double duration_ms = 0.0;
    std::size_t msg_index = 0; // 关联转录消息索引（跳转用）
};

struct TaskLite {              // TaskManager 任务条目
    std::string name;
    std::string status;        // Pending/Running/Completed/Cancelled/Failed
    float progress = 0.0f;
};

struct DiffLine {              // 行级 diff 单元
    enum class Kind { Equal, Insert, Modify } kind;
    std::string text;
    int line_no = 0;           // 新文件坐标行号（内联高亮定位用）
};

struct FileChange {            // 会话内一次文件修改
    std::string file_path;
    std::string purpose;       // 修改目的（该步 reasoning 摘要）
    std::string old_string;    // Edit 旧内容（Write 全量改写时为空）
    std::string new_string;    // 新内容
    int64_t timestamp = 0;
    std::size_t msg_index = 0; // 关联转录消息（跳转用）
    std::vector<DiffLine> diff;  // 行级 diff（仅 Equal/Insert/Modify）
    int new_start = 0;           // 修改区块在文件中的起始行
};

struct FileViewState {         // 文件 tab 状态（纯内容视图）
    std::string path;          // 当前查看文件（空=未打开）
    std::vector<std::string> lines;   // 当前内容（按行）
    std::string lang;          // 高亮语言（扩展名推断）
    int scroll = 0;            // 首行索引
    bool dirty = false;        // /edit 后需重读
    std::vector<FileChange> changes;  // 该文件会话内修改（内联高亮用）
};

struct ChangeViewState {       // 变更记录 tab 状态
    std::vector<FileChange> changes;  // 会话内全部修改（按文件分组）
    int selected = 0;          // 选中修改点（-1=无）
    bool purpose_expanded = false;    // e 展开完整 reasoning
};

struct SidebarTabsModel {
    SidebarTab active = SidebarTab::kTasks;
    bool changes_open = false;   // 变更记录 tab 是否打开（有 FileChange 时自动开）
    bool file_open = false;      // 文件 tab 是否打开（/view 打开）
    // —— 任务调度 ——
    bool busy = false;
    std::string current_tool;  // 当前执行中的工具名
    int step_number = 0;
    int total_steps = 0;
    std::vector<SubAgentLite> sub_agents;
    std::vector<TaskLite> background_tasks;
    // —— 文件 / 变更记录 ——
    FileViewState file;
    ChangeViewState changes;
};
```

---

## 3. 任务调度 tab（可交互）

### 3.1 区块与数据源

| 区块 | 内容 | 数据源 |
| --- | --- | --- |
| 状态行 | `● 生成中 / 空闲` + 模型名 | `ActionSetBusy`、`sidebar.model` |
| 当前工具 | 最近一次 BeginTool 未配对的 EndTool | `ActionBeginTool` / `ActionEndTool` |
| 步骤进度 | 当前轮 step / total | `ActionStepDone`、`ActionTurnDone` |
| 子 Agent | 按 task_id 聚合的列表（**可交互**） | `ActionSubAgentProgress` / `ActionSubAgentCompleted` |
| 后台任务 | TaskManager 任务列表 | 只读查询 `ITaskManager::getRunningTasks()`（原子字段） |
| 会话统计 | 上下文/token/成本/权限 | 现有 `SidebarModel` 字段（原侧栏内容移入本 tab 底部） |

### 3.2 交互

- 子 Agent 列表用 `ftxui::Menu`（纵向，可聚焦），选中项 `❯` 标记。
- **Enter**：跳转转录区到该子 Agent 的消息位置。
  - 跳转实现：`transcript` 暴露 `scroll_to_message(msg_index)`，内部 `scroll_top = prefix[msg_index]`、`m_follow = false`（复用现有前缀和 O(1) 定位）。
  - `msg_index` 在 `ActionSubAgentProgress` 到达时记录（当时转录消息数）。
- 后台任务列表只读展示（进度条复用 `build_context_gauge` 风格）。

---

## 4. 文件 tab（/view 只读查看）

### 4.1 打开流程（`/view <file>` 命令）

```
composer 输入 "/view src/main.cpp" + Enter
  → run_command → LocalCommand("view").call(args)
  → App::cmd_view(args)
      1. 解析路径（相对路径 + @ 引用）
      2. 读文件（≤2MB，超限截断提示）→ 按行切分
      3. 推断语言（扩展名 → lang）
      4. 收集该文件会话内 FileChange（来自修改追踪）
      5. file_open = true，active = kFiles，RequestAnimationFrame 重绘
```

### 4.2 渲染（纯内容视图 + 内联 diff 高亮）

```
┌─────────────────────────────┐
│ src/main.cpp · 128 行 · C++ │  ← 路径栏
├─────────────────────────────┤
│  12  int main() {            │
│  13    auto x = foo();       │
│ +14  auto y = bar();         │  ← 新增/修改行（绿 + 高亮）
│  15  return x;               │
│  16  }                       │
├─────────────────────────────┤
│  ↑↓ 滚动 · Esc 关闭          │  ← 状态栏
└─────────────────────────────┘
```

- **行号列**：右对齐，`TextFaint` 色。
- **内联 diff 高亮**：逐行比对 `FileChange.diff`，新增/修改行加 `+` 标记（绿 / Accent 底），**不渲染删除行**。
- **虚拟化滚动**：文件行高固定 1，`scroll` 偏移 + 可视行数切片（比 transcript 简单）。
- 无上下两区、无修改点列表——文件 tab 只负责"看文件 + 看改动落在哪"。

### 4.3 交互

- `↑↓ / PgUp / PgDn`：滚动。
- `Esc`（或点 tab 栏 `✕`）：关闭文件 tab，`file_open = false`，返回 `kTasks`。
- 再次 `/view`：若已打开同一文件则仅切到该 tab；不同文件则重读并定位。
- `/edit` 返回后置 `file.dirty = true`，下次切到文件 tab 重读（保持与磁盘一致）。

---

## 5. 变更记录 tab（内联 diff Review）

### 5.1 内容

- 会话内全部 `FileChange`，按文件分组、按时间排序。
- 首个 FileChange 出现时 `changes_open = true`（tab 自动出现并高亮提示新修改）。
- 每个修改点渲染为 git diff 风格 hunk：

```
┌─────────────────────────────┐
│ src/main.cpp · 3 处修改      │  ← 文件头
├─────────────────────────────┤
│ ❯ ⚑ 改用 bar() 计算 y       │  ← 修改点（Menu 选中）
│   ── 目的：改用 bar() 计算 y │  ← 目的（默认一行，e 展开）
│   +14  auto y = bar();       │  ← hunk：+ 新增/高亮修改行
│   +15  return y;             │
│   ⚑ 提取常量 kMaxRetry       │
│   ⚑ 修复空指针判断           │
├─────────────────────────────┤
│  ↑↓ 选择 · e 展开 · Enter    │  ← 状态栏
└─────────────────────────────┘
```

- **修改点列表**：`ftxui::Menu`（纵向），每项 = 目的摘要（一行）+ 文件/行号；选中项 `❯` 标记。
- **hunk 视图**：当前选中修改点的 diff 行（`+` 新增 / 高亮修改行），仅展示新内容行，不渲染删除行。
- **修改目的**：选中修改点上方一行 Dim 注释（reasoning 摘要，截断 ~40 字符）；按 `e` 展开完整 reasoning 多行。

### 5.2 交互

- `↑↓`：在修改点间移动（跨文件）。
- `e`：展开/收起当前修改点的完整 reasoning。
- **Enter**：跳转「文件」tab 并滚动到该修改点对应行（`scroll = new_start`，`file_open = true`，`active = kFiles`）。
- `Enter`（文件 tab 内）：跳转转录区到该修改点对应消息（`scroll_to_message`）。
- `Esc`（或点 tab 栏 `✕`）：关闭变更记录 tab，`changes_open = false`，返回 `kTasks`。

---

## 6. 数据流与事件

```
后台线程                         UI 线程（ViewModel）
─────────                        ─────────────────
ActionBeginTool(Edit/Write) ──►  apply_variant:
                                   解析 arguments → FileChange
                                   快照 reasoning → purpose
                                   计算行级 diff（line_diff）
ActionSubAgentProgress ──────►  apply_variant:
                                   聚合 SubAgentLite + 记录 msg_index
ActionTurnDone ──────────────►  apply_variant:
                                   更新 step/token 统计
TaskManager（只读查询） ──────►  渲染时 getRunningTasks()
```

- 全部沿用现有 `EventBridge → ActionQueue → ViewModel` 单线程模型，**不新增事件类型**，只在 `apply_variant` 内补累积逻辑。
- 后台任务列表为渲染时只读查询（TaskManager 字段原子化，无锁安全）。
- `line_diff` 在 `apply_variant(ActionBeginTool)` 内同步计算（Edit/Write 的 old/new 通常很小，O(n·m) 可接受）。

---

## 7. 代码结构清单

| 文件 | 改动 |
| --- | --- |
| `core/utils/line_diff.h/.cpp` | **新增**：行级 LCS diff（纯函数，可单测）✅ P4 |
| `widgets/sidebar_tabs.h/.cpp` | **新增**：tab 栏 + 任务调度视图（含子 Agent Menu）✅ |
| `widgets/file_viewer.h/.cpp` | **新增**：文件查看视图（行号 + 内联 diff 高亮 + 滚动）✅ P3/P4 |
| `widgets/change_viewer.h/.cpp` | **新增**：变更记录视图（修改点 Menu + hunk + 目的展开）✅ P5 |
| `vm/view_model.h/.cpp` | 扩展 `SidebarTabsModel`；`apply_variant` 补修改追踪/子 Agent 聚合 ✅（FileChange 追踪 P4） |
| `theme/strings.h` | 新增 tab 标签、查看器提示、diff 标记文案 ✅ |
| `command/builtins.h/.cpp` | 新增 `on_view` 回调 + 注册 `/view` ✅ |
| `render/transcript_layout.h` | 新增 `scroll_to_message` 辅助（或 App 侧直接算 `scroll_top`）✅（jump_to_sub_agent 直接算） |
| `app.h/.cpp` | `cmd_view()`、tab 切换、`CatchEvent` 处理查看器按键、接线 ✅ |

---

## 8. 分阶段实施

| 阶段 | 目标 | 关键动作 | 验收 |
| --- | --- | --- | --- |
| P1 | 侧边栏 tab 化骨架 | `SidebarTabsModel` + 自绘 tab 栏（含 ✕ 关闭）+ 三 tab 空壳；原侧栏内容移入任务调度 tab 底部 | ✅ 启动默认任务调度，tab 可切换，✕ 可关闭变更记录/文件，原信息不丢 |
| P2 | 任务调度 tab 数据 + 交互 | 聚合 busy/工具/步骤/子 Agent/后台任务；子 Agent Menu + Enter 跳转转录 | ✅ 生成中能看到当前工具与子 Agent；Enter 跳转正确 |
| P3 | `/view` 打开 + 只读滚动 | `cmd_view()` + 文件读取 + 行号 + 虚拟化滚动 | ✅ `/view` 打开文件，行号/滚动正常 |
| P4 | 修改追踪 + 内联 diff | `line_diff` + FileChange 追踪 + 文件 tab 内联高亮 | ✅ Edit/Write 后 `/view` 显示新增/修改行标记 |
| P5 | 变更记录 tab | `change_viewer`：修改点 Menu + hunk + 目的展开 + Enter 跳转文件 tab | ✅ 变更记录列出全部修改，选中跳转正确 |
| P6 | 收尾 | Esc 关闭、文案入 strings、补测试（`/edit` 联动重读属方案 B 另行规划） | ✅ 全流程可用 |

---

## 9. 测试计划

| 目标 | 用例 |
| --- | --- |
| `line_diff`（纯函数） | 空输入 / 全新增 / 全删除 / 交错修改 / 大文件降级 |
| `view_model.apply` | BeginTool(Edit/Write) 生成 FileChange + purpose 截断 + diff 计算；SubAgentProgress 聚合与 msg_index |
| `select_transcript_slice` 扩展 | `scroll_to_message` 定位正确（前缀和边界） |
| 渲染（无头） | 行号对齐、内联 diff 标记行、hunk 视图、目的注释截断、tab 栏 ✕ 布局 |
| 交互（无头） | tab 切换、✕ 关闭/重开、变更记录选中跳转文件 tab、Enter 跳转转录 |

---

## 10. 风险与边界

| # | 风险 | 缓解 |
| --- | --- | --- |
| R1 | reasoning 缓冲在 BeginTool 时可能为空（无思考直接调工具） | purpose 回退为工具参数摘要（new_string 首行） |
| R2 | 大文件 diff O(n·m) 超时 | 行数阈值（如 >5000 行）降级为整块标记 |
| R3 | 子 Agent msg_index / FileChange 在切换会话后失效 | 会话切换时清空 `sub_agents` 与 `file_changes`（复用 `cancel_and_wait` 时机） |
| R4 | `/view` 与 `/edit` 并发（编辑中打开查看器） | `/view` 读取前 `cancel_and_wait_current_task()` 或标记 dirty 延迟重读 |
| R5 | 后台任务查询与 TaskManager 生命周期 | 渲染时 try 捕获；TaskManager 单例常驻，风险低 |
| R6 | 变更记录跨文件跳转（文件 tab 需切换文件） | `cmd_view` 复用：跳转时若目标文件未打开则先读入再定位行 |
| R7 | 3 个 CJK tab + 边框总宽约 36-40 列，超过 30 列侧栏 | 当前仅 1-2 tab 打开（P3 阶段），不溢出；P4/P5 实现后需加宽侧栏（如 34-36 列）或压缩 tab 宽度（待定） |

---

## 11. 开放问题（已确认）

1. ~~文件 tab 是否保留内联 diff 高亮~~ → **保留**（新增/修改行标记，纯内容视图无列表）。
2. ~~变更记录 tab 内容形态~~ → **内联 diff 视图**（git diff 风格 hunk + 目的注释，选中跳转文件 tab）。
3. ~~新 tab 名称~~ → **变更记录**。
4. ~~删除行展示~~ → **仅高亮新增/修改行**（删除内容以修改点摘要呈现）。
5. ~~修改目的粒度~~ → **可展开多行**（默认一行，`e` 展开完整 reasoning）。
