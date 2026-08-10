# 运行中打断（Interrupt）生命周期设计

> 关联 Issue：#23 [讨论] 运行中打断（interrupt）生命周期
> 状态：方案设计（未实现）
> 日期：2026-08-10

## 1. 目标与语义统一

一次"打断"是一个显式动作，语义一致地作用于三类运行中对象，且**绝不丢会话记录、绝不留半写文件**：

| 运行中对象 | 打断动作（期望） |
|---|---|
| LLM 流式回复 | 立即断开请求，保留已生成 partial 内容并持久化 |
| bash 长任务（同步） | 协作式取消 → 必要时销毁 POSIX 进程树（含子孙） |
| FileEdit / FileWrite | 在取消点中止，文件保持原子/一致性（原文件不损坏） |

按键语义收敛（force 仅用于强退）：
- **Ctrl+C 单次 / Esc 单次** = 打断当前操作（`InterruptEvent{.force=false}`）
- **1s 内连按 / Esc 长按 1s** = 强制退出（`InterruptEvent{.force=true}` + `ShutdownEvent`，维持现状）

## 2. 现状审计（已确认的关键缺口）

### 2.1 中断数据/控制流（现状）
```
Ctrl+C ──> terminal.cpp:322  publish InterruptEvent{.force=false}
              │
              ▼
ChatSession::subscribe_interrupt (chat_session.cpp:1162)
              │ 仅 m_provider->interrupt()   ← 断开 LLM HTTP 流
              ▼
ReActLoop.run(should_cancel)                 ← should_cancel = Task::m_should_cancel
  ├─ Thought: execute_thought 检查 should_cancel / reader Cancelled ✓
  ├─ Action:  ctx.cancel_flag = &should_cancel（注入工具）✓
  ├─ 工具等待: exec.future.get()             ← ❌ 阻塞，无取消检查
  └─ Observation: 推送 tool_result
                    │
                    ▼
ChatSession: was_interrupted → partial 持久化 ✓（chat_session.cpp:763-792）
```

### 2.2 根因缺口（Issue #23 对应）

**A. 运行时中断不置位 should_cancel（最严重）**
`subscribe_interrupt` 只调 `m_provider->interrupt()`。`m_current_task->cancel()`（置 `should_cancel`）只在 `~ChatSession` 调用（chat_session.cpp:210）。
→ 工具执行期间（bash 长任务）用户打断：工具的 `is_cancelled()` 恒 false → subprocess 不 kill → `future.get()` 无限阻塞 → **打断完全失效、UI 卡死**。

**B. ReActLoop 工具等待阶段不可中断**
`react_loop.cpp:636` `exec.future.get()` 无超时/无取消检查。中断信号即便到位，也要等所有工具完成。

**C. bash 只 kill 直接 pid**
`subprocess.cpp:549-563`：`kill(pid, SIGTERM)` 只发 shell。`bash -c "sleep 1000"` 的 `sleep` 成孤儿继续跑（资源/副作用残留）。

**D. FileEditTool 无取消点 + 非原子写**
`file_edit_tool.cpp:517` `std::ofstream(trunc)` 直写，无 `is_cancelled()` 检查点、无 temp+rename 原子替换。大文件打断/崩溃 → 半写损坏。

**E. 取消源分散**
现存在 2 个取消布尔：`InterruptEvent.force`（输入侧）+ `Task::m_should_cancel`（运行侧），外加 `ToolContext::cancelled_` 内部 fallback。层级间未打通。

## 3. 设计

### 3.0 核心原则
**不改输入协议、不引入新取消类型** —— 复用 `Task::m_should_cancel` 作唯一运行时取消源，`InterruptEvent` 到达时同时驱动"断开 LLM 流 + 置位 should_cancel"两条腿。避免翻新 `CancellationToken` 带来的大面积改动。

### 3.1 ChatSession：运行中中断置位 should_cancel（修复根因 A）
`subscribe_interrupt`（chat_session.cpp:1162）改为：
```cpp
m_interrupt_token = m_event_bus.get().subscribe<InterruptEvent>(
    [this](const InterruptEvent&) {
        // 1) 快速断开 LLM 流（保留现状）
        if (m_provider) m_provider->interrupt();
        // 2) 置位 should_cancel → ReActLoop + 工具协作取消生效（新增）
        std::lock_guard<std::mutex> lock(m_state_mutex);
        if (m_current_task) m_current_task->cancel();
    });
```
- 加锁原因：`m_current_task` 由 `m_state_mutex` 保护（析构也读写）。
- 效果：bash 的 `opts.is_cancelled` 立即返回 true → subprocess kill；`ToolExecutor::execute` 的 `ctx.is_cancelled()` 立即生效；下一轮 Thought 检查 `should_cancel` 立即 Cancelled。

### 3.2 ReActLoop：工具等待阶段可中断（修复缺口 B）
`react_loop.cpp:633-665` 等待循环改造：
```cpp
for (auto& exec : executions) {
    // 协作式等待：工具会响应 should_cancel 快速退出；等待过程可感知取消
    while (exec.future.wait_for(std::chrono::milliseconds(100))
           != std::future_status::ready) {
        if (should_cancel) {
            // 工具正在协作退出中，继续等待其返回（最多等工具自身退出）
            // bash 被杀后 executor 返回 Cancelled → future ready
        }
    }
    auto [result_text, tool_error] = exec.future.get();
    // ... Observation 照常生成（tool_result 带 Cancelled 语义）
}
```
- 保持"等待至完成"保证：Observation 一定生成、`persist_messages_range` 逻辑（chat_session.cpp:748）在 loop.run 返回后必然执行 → **消息不丢**。
- 若工具不协作退出（极端：卡死 I/O），受 3.3 的 SIGKILL 升级保护（≤ ~5s）。

### 3.3 BashTool / subprocess：进程树销毁（修复缺口 C）
POSIX（subprocess.cpp 子进程侧 + 父进程 kill 侧）：
1. **fork 子进程**：`setpgid(0, 0)` 建立独立进程组（迁移到 execvp 前）。
2. **取消/超时**：`kill(-pid, SIGTERM)`（负 pid = 进程组）→ 5s 后 `kill(-pid, SIGKILL)` 升级。
   - 覆盖 `bash -c "sleep 1000"`：sleep 与 bash 同组，一并销毁。
3. **waitpid 复用现有升级逻辑**（仅信号目标从 pid 改为 -pid）。
Windows：维持 `TerminateProcess`；如需防子进程逃逸可加 Job Object（低优先级，暂缓）。

### 3.4 FileEditTool / FileWriteTool：原子写 + 取消点（修复缺口 D）
1. **原子写**：`write_file_with_encoding` 改为先写临时文件（同目录 `.<name>.<pid>.tmp`）→ `fs::rename` 原子替换 → 写后更新 mtime/FileReadStateTracker 不变。
2. **取消点**：在 create_backup → 写临时 → rename 之间插入 `ctx.is_cancelled()` 检查：
   - 已取消 → 删除临时文件、保留备份、返回 `Error{Code::Cancelled}`。
   - 原文件从未被部分覆盖（rename 原子性保证）。
3. 大文件慢 IO 场景下，检查点可放在临时写入过程中（按写入 chunk 轮询，可选）。

### 3.5 会话记录持久化保证（Issue 要求 1）
依赖现有路径 + 3.1/3.2 保证 loop.run 完整返回：
- user 消息：入队即 `persist_message`（chat_session.cpp:682）。
- 工具阶段产生的 assistant(tool_use)/tool_result：`persist_messages_range`（line 748）。
- 打断产生的 partial：`was_interrupted` 分支 `persist_message`（line 774）。
- **新增保证**：中断置位 should_cancel 后 ReActLoop 一定从规范分支退出（Thought Cancelled 或工具 Cancelled），而非卡在 `future.get()`，故上述持久化必然可达。

## 4. 分阶段落地

| 阶段 | 内容 | 验收 |
|---|---|---|
| **P1 核心修复** | 3.1 + 3.2 + 3.3 | ① `bash -c "sleep 1000"` 执行中打断，≤~5s 内进程树全清、UI 立即响应；② 打断后 JSONL 含 user + partial + 已完成工具消息 |
| **P2 文件工具一致性** | 3.4 | 大文件写入中打断 → 原文件完整/新内容未部分落盘；取消点单测 |
| **P3 语义收敛 + 后台任务联动** | Esc 单次=打断；`run_in_background=true` 任务取消联动（发布 TaskCancelledEvent → 杀后台进程组） | Esc 打断表现与 Ctrl+C 一致；后台任务取消后进程组销毁、输出注册表清理 |

## 5. 测试计划

| 测试 | 文件 | 断言 |
|---|---|---|
| ReActLoop 工具执行中置 should_cancel | `tests/unit/agent/.../test_react_loop_interrupt.cpp` | loop 在工具返回后走 Cancelled 分支，`was_interrupted=true`，messages 含 tool_result |
| ChatSession InterruptEvent → task cancelled | 扩展 `test_chat_session` | 发布 InterruptEvent 后 `task->shouldCancel()==true`；partial 消息被 persist |
| subprocess 进程树销毁 | 扩展 `test_subprocess` | fork+setpgid 后取消：shell 与 `sleep` 子进程均退出，`ExecOutput.cancelled==true` |
| FileEdit 取消原子性 | 扩展 `test_file_edit_tool` | 写中间置 is_cancelled：原文件内容不变；无残留临时文件 |
| 语义回归 | `test_tui_state` 等 | force=false 不触发 Shutdown；force=true 仍强退 |

## 6. 风险与权衡

- **fork+setpgid 顺序**：必须在 execvp 前 setpgid，父进程 side 用 `kill(-pid,...)`；需小心 setpgid 与 waitpid 竞态（子进程已退出时 `kill(-pid)` 返回 ESRCH，兼容处理）。
- **future.get() 阻塞时长**：仍受 subprocess 5s SIGKILL 升级约束，极端卡死场景 → 引入 `wait_for` 上限（建议默认 10s）后强制放弃等待并脱离（detach），后续 P3 评估。
- **原子写对符号链接/权限**：rename 会替换目标 inode，对符号链接目标需先解析；POSIX rename 需同目录（跨目录返回 EXDEV，改用同名临时文件策略）。
- **should_cancel 被置位后不可复位**：当前 turn 内不可恢复；保持 ReAct 状态下一次 send_message 新建 task（新 should_cancel），符合现有语义（一次打断结束本轮）。