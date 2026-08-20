# 方案 B：/edit 内嵌 nvim 规划

> 状态：已实现（2026-08-20）
> 范围：`src/ftxtui`、`src/core/process`
> 基线：`docs/plans/2026-08-20-sidebar-tabs-file-review.md`（方案 A `/view` 已落地）
> 前置探讨：方案 A（`/view` 只读查看器）与方案 B（`/edit` 内嵌 nvim）——本计划落地方案 B

---

## 0. 决策记录

| 决策点 | 结论 | 说明 |
| --- | --- | --- |
| 编辑器 | **nvim**（Neovim） | 跨平台、终端原生、可脚本化；`nvim <file>` 即可进入编辑 |
| 内嵌机制 | **FTXUI `App::WithRestoredIO`** | 官方提供的「临时卸载终端钩子 → 执行回调 → 恢复」机制，专为启动外部交互程序设计 |
| 子进程方式 | **新增 `exec_interactive()`（不捕获 stdio）** | 现有 `subprocess::exec()` 用管道捕获输出，nvim 会退化为非交互模式；必须继承终端 stdio |
| 编辑期间模型 | **编辑前 `cancel_and_wait_current_task()`** | 避免模型后台写文件与用户手动编辑冲突（复用会话切换的既有防竞争机制） |
| 编辑返回 | **重读文件 + 刷新文件 tab** | 复用 `cmd_view` 读取逻辑；`FileViewState::dirty` 标志联动 |
| 变更记录联动 | **编辑产生的修改记录为 FileChange（可选增强）** | 复用 `line_diff` + `track_file_change`；本期先做重读，联动列为 E4 |
| 平台 | Windows 10（主）+ POSIX | `exec_interactive()` 双平台实现；Windows 上 nvim 需在 PATH |

---

## 1. 目标与范围

### 1.1 目标

1. `/edit <file>` 命令：在 TUI 内直接拉起 nvim 编辑文件，退出后回到 TUI。
2. 编辑返回后自动重读文件，文件 tab 显示最新内容（与磁盘一致）。
3. 编辑期间模型活动暂停，避免与手动编辑冲突。
4. nvim 缺失或文件异常时给出明确提示，不破坏 TUI 状态。

### 1.2 非目标（本期不做）

- 内嵌终端模拟器（libvterm 等）——`WithRestoredIO` 全屏切换已满足需求，无需终端模拟。
- 编辑器选择器（默认 nvim，不提供 vim/emacs 切换）。
- 编辑会话历史（编辑前后 diff 的持久化）。
- 方案 A 之外的文件树浏览器。

---

## 2. 核心机制：WithRestoredIO

FTXUI 7.0.3 的 `App::WithRestoredIO(Closure)` 返回一个闭包，执行时依次：

1. `Internal::Uninstall()`：恢复终端原始状态（退出 raw mode、恢复 Windows console mode、恢复 alternate screen）。
2. `fn()`：执行回调——此处同步启动 nvim 并等待其退出。
3. `Internal::Install()`：重新安装 FTXUI 终端钩子，恢复 TUI。

```cpp
// App 内（UI 线程）：
auto edit = m_screen.WithRestoredIO([&] {
    agent::process::exec_interactive("nvim", {file_path});  // 同步等待退出
});
edit();  // 必须 UI 线程执行（Uninstall/Install 操作终端）
m_screen.RequestAnimationFrame();
```

约束：

- 闭包必须在 **UI 线程**执行（`Uninstall`/`Install` 直接操作终端句柄）。
- `WithRestoredIO` 执行期间 FTXUI 主循环被阻塞（同步等待 nvim），这是预期行为——编辑是模态操作。
- 编辑前需先 `cancel_and_wait_current_task()`，否则模型后台线程可能在编辑期间写同一文件。

---

## 3. 命令设计：/edit

### 3.1 命令注册

沿用 `builtins.cpp` 模式，新增 `on_edit` 回调：

```cpp
// builtins.h
std::function<void(const std::string&)> on_edit;  ///< /edit：内嵌 nvim 编辑文件

// builtins.cpp
auto edit_cmd = agent::command::make_local_command("edit", std::string(str::kCmdEditDesc));
edit_cmd->set_argument_hint("/edit <file>");
edit_cmd->set_call([on_edit = cb.on_edit](const std::string& args, const CommandContext&) {
    if (on_edit) on_edit(args);
    return CommandResult::ok("");
});
registry.register_command(edit_cmd);
```

### 3.2 打开流程（`App::cmd_edit(args)`）

```
composer 输入 "/edit src/main.cpp" + Enter
  → run_command → LocalCommand("edit").call(args)
  → App::cmd_edit(args)
      1. 解析路径（相对路径 + @ 引用，复用 cmd_view 的解析逻辑）
      2. 校验文件存在（不存在 → 提示，不进入编辑）
      3. 检测 nvim（PATH 查找；缺失 → 提示安装，不进入编辑）
      4. cancel_and_wait_current_task()（暂停模型活动）
      5. 打开文件 tab 显示当前内容（复用 cmd_view 读取 + 行号 + 内联 diff）
      6. WithRestoredIO 同步启动 nvim，等待退出
      7. 编辑返回：重读文件 → 刷新文件 tab → dirty 清除
      8. 若内容变化：可选记录 FileChange（E4）
```

### 3.3 交互与提示

- 编辑期间 TUI 完全让出终端（用户只看到 nvim）。
- 编辑退出（`:wq` / `:q`）后自动回到 TUI，文件 tab 显示最新内容。
- nvim 缺失提示：`（未找到 nvim，请安装 Neovim 并加入 PATH）`。
- 文件不存在提示：复用 `kViewNotFound` 风格文案。

---

## 4. 状态模型

`FileViewState` 已预留 `dirty` 字段（方案 A 注释「/edit 后需重读」），本期启用：

```cpp
struct FileViewState {
    std::string path;
    std::vector<std::string> lines;
    std::string lang;
    int scroll = 0;
    bool dirty = false;   ///< /edit 返回后置 true，下次切到文件 tab 重读
    std::vector<FileChange> changes;
};
```

编辑返回后流程：

1. `cmd_edit` 重读文件，更新 `lines` / `lang` / `scroll`。
2. 若文件 tab 已打开同一文件：直接刷新内容（保持滚动位置或回到顶部）。
3. 若文件 tab 未打开：`file_open = true`、`active = kFiles`，显示最新内容。
4. `dirty` 标志用于「编辑返回但未切到文件 tab」的场景：下次切到文件 tab 时重读。

---

## 5. 数据流与事件

```
用户输入 "/edit <file>" + Enter
  → CommandExecutor → App::cmd_edit
  → cancel_and_wait_current_task()          // 暂停模型
  → cmd_view(file)                          // 打开文件 tab 显示当前内容
  → WithRestoredIO { exec_interactive(nvim) }  // 全屏编辑（模态）
  → 编辑返回
  → reload_file(file)                       // 重读 + 刷新文件 tab + dirty 清除
  → （E4）对比旧/新内容 → FileChange → 变更记录 tab
```

不新增事件类型；`cmd_edit` 在 UI 线程同步执行，编辑期间无事件泵活动（模态）。

---

## 6. 代码结构清单

| 文件 | 改动 |
| --- | --- |
| `core/process/subprocess.h/.cpp` | **新增** `exec_interactive()`：交互式子进程（不捕获 stdio，继承终端） |
| `command/builtins.h/.cpp` | 新增 `on_edit` 回调 + 注册 `/edit` |
| `vm/view_model.h` | 启用 `FileViewState::dirty`（已有字段） |
| `app.h/.cpp` | `cmd_edit()`、`reload_file()`、`WithRestoredIO` 接线、`CatchEvent` 无新增按键 |
| `theme/strings.h` | `/edit` 描述、nvim 缺失提示、编辑返回提示文案 |

---

## 7. 分阶段实施

| 阶段 | 目标 | 关键动作 | 验收 |
| --- | --- | --- | --- |
| E1 | `exec_interactive()` | 跨平台交互式子进程（Windows `CreateProcessW` 继承 stdio / POSIX `fork+execvp`）+ 单元测试 | ✅ 能启动 nvim 并正常交互，退出码正确 |
| E2 | `/edit` 基础流程 | `on_edit` 回调 + `cmd_edit()`（解析/校验/nvim 检测/`WithRestoredIO` 启动） | ✅ `/edit` 拉起 nvim，`:wq` 后回到 TUI |
| E3 | 编辑返回联动 | `reload_file()` 重读 + 文件 tab 刷新 + `dirty` 清除 | ✅ 编辑保存后文件 tab 显示最新内容 |
| E4 | 变更记录联动 | 编辑前后 diff → FileChange → 变更记录 tab | ✅ 手动编辑出现在变更记录，可跳转 |
| E5 | 收尾 | 文案入 strings、错误提示、补测试、更新文档 | ✅ 全流程可用 |

---

## 8. 测试计划

| 目标 | 用例 |
| --- | --- |
| `exec_interactive()` | 启动成功/失败（命令不存在）、退出码透传、stdio 继承（交互式命令可运行） |
| `cmd_edit` 路径解析 | 相对路径、@ 引用、空参数、文件不存在 |
| nvim 检测 | PATH 中存在/缺失 → 对应行为（缺失提示，不进入编辑） |
| 编辑返回重读 | 编辑后内容变化 → 文件 tab 刷新；未变化 → 不抖动 |
| 变更记录联动（E4） | 编辑产生的修改生成 FileChange，`line_diff` 正确 |

---

## 9. 风险与边界

| # | 风险 | 缓解 |
| --- | --- | --- |
| R1 | `subprocess::exec()` 捕获 stdio 导致 nvim 非交互 | 新增 `exec_interactive()` 继承终端，不捕获输出 |
| R2 | `WithRestoredIO` 在 Windows 上恢复 console mode 不完整 | 复用 FTXUI 官方 `Uninstall`/`Install`；E2 阶段在 Windows 实测验证 |
| R3 | 编辑期间模型后台写文件冲突 | 编辑前 `cancel_and_wait_current_task()`（复用会话切换机制） |
| R4 | nvim 未安装 | 编辑前 PATH 检测，缺失提示并中止，不进入编辑 |
| R5 | 大文件编辑后重读性能 | 复用 `cmd_view` 的 2MB 截断保护 |
| R6 | 编辑异常退出（kill / 崩溃） | `exec_interactive` 返回非零退出码 → 仍重读文件（内容以磁盘为准），提示编辑未正常保存 |
| R7 | `WithRestoredIO` 闭包误在后台线程执行 | `cmd_edit` 仅在 UI 线程调用；代码注释明确约束 |

---

## 10. 开放问题（已决策）

1. 编辑返回后是否自动切到文件 tab？——**已决策**：默认切到（`cmd_view` 打开文件 tab），用户刚编辑完期望看到结果。
2. 编辑产生的修改是否记录为 FileChange？——**已决策**：E4 落地，`reload_file` 对比编辑前后内容，差异生成 `FileChange`（purpose="手动编辑"）并打开变更记录 tab。
3. nvim 是否支持额外参数（如 `-c 'set number'`）？——**已决策**：本期仅 `nvim <file>`，参数扩展后续再说。
4. 是否提供「编辑中」状态提示？——**已决策**：编辑是模态全屏切换，TUI 不可见，无需状态提示；编辑返回后按退出码提示「已保存」/「未正常保存」。
