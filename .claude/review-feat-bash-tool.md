# Code Review 报告 — feat/bash-tool

**审查范围：** `feat/bash-tool` → `develop`，commit `60ecbcb`
**对比基线：** `73fc1a3`（含 feat/sandbox 合并及修复 d084d76）
**审查立场：** 假设改动有错误，反向举证
**审查结论：** ✅ APPROVED（建议合并，附 Medium 建议）

## 概览

| 项 | 统计 |
|----|------|
| 文件变更 | 14 files / +1245 / -30 |
| 新增测试 | 15 cases / 35 assertions（[bash] 标签） |
| 测试总数 | 587 cases / 2072 assertions（全通过） |
| mergeable | MERGEABLE |
| 构建 | pass（MSVC Debug，无 warning） |
| 关联 Issue | 无 |
| 消费方 | ✅ factory.cpp 注册 + chat_session.cpp DI 注入 + chat_renderer.cpp 事件订阅 |

## 验收标准核对

无关联 Issue，按 commit message 声明的交付项核对：

| 交付项 | 状态 | 说明 |
|------|------|------|
| BashTool Shell 执行 | ✅ | [bash_tool.cpp](file:///d:/develop/Workspace/workx/src/agent/tool/BashTool/bash_tool.cpp) 同步路径 execute_sync() |
| 后台任务执行 | ✅ | execute_background() 通过 TaskManager.launch() |
| 沙盒包装 | ✅ | 集成 SandboxAdapter::wrap_command() |
| 超时设置 | ✅ | kDefaultTimeoutMs=120s, kMaxTimeoutMs=600s |
| 进度回调 | ✅ | ctx.report_progress() 在开始/完成/降级时上报 |
| ToolContext 扩展 | ✅ | [context.h](file:///d:/develop/Workspace/workx/src/agent/tool/context.h) 新增 task_manager_ptr + task_manager() |
| ReActLoop DI 注入 | ✅ | [react_loop.h#L186](file:///d:/develop/Workspace/workx/src/agent/core/react_loop.h#L186) 构造函数新增 ITaskManager* |
| ChatRenderer 事件订阅 | ✅ | [chat_renderer.cpp#L778-L802](file:///d:/develop/Workspace/workx/src/tui/render/chat_renderer.cpp#L778-L802) TaskCompleted/TaskFailed |
| factory 注册 | ✅ | [factory.cpp#L167](file:///d:/develop/Workspace/workx/src/app/factory.cpp#L167) |
| 单元测试 | ✅ | 15 cases 覆盖元信息/校验/同步/超时/取消/后台/进度/沙盒 |

## 七维变更正确性检测

### 4.1 契约一致性 — ✅

- input_schema 与 prompt 描述一致（command/description/timeout/run_in_background/dangerously_disable_sandbox）
- ToolContext 新增 task_manager_ptr 遵循已有 config_manager_ptr 的 DI 模式（nullptr 抛异常）
- ReActLoop 构造函数新增 ITaskManager* 参数有默认值 nullptr，不破坏已有调用方
- input_schema 中 `cwd` 字段未声明但 call() 中读取 — 见 L-1

### 4.2 并发与生命周期 — ✅

- 同步路径：cancel_flag 通过 ExecOptions.is_cancelled 传递给 subprocess，实现即时取消
- 后台路径：TaskManager 的 should_cancel 引用被 lambda 捕获，传递给 ExecOptions.is_cancelled
- lambda 捕获方式：`[command, cwd, timeout_ms, wrapped]` 值捕获，生命周期独立于 ToolContext
- ChatRenderer 事件订阅在 stop() 中正确取消订阅

### 4.3 错误处理 — ⚠️

- 同步路径：exec 启动失败返回 err，非零退出码返回 ok（含 exit_code），超时/取消返回 ok（含标志）
- **后台路径：exec 返回 err 被丢弃**（见 M-1）
- 参数校验完整：缺 command/空 command/非字符串 command 均有明确错误码

### 4.4 设计与可测试性 — ✅

- BashTool 与 SandboxAdapter 解耦，通过 wrap_command() 接口交互
- 同步/后台路径分离，职责清晰
- 测试覆盖全面：15 cases 覆盖元信息/校验/同步执行/超时/取消/后台/进度/沙盒
- 测试使用跨平台命令（echo/exit/ping/sleep）

### 4.5 回归风险 — ✅

- ReActLoop 构造函数新增参数有默认值，不破坏已有调用方
- ToolContext 新增字段有默认值，不破坏已有结构体初始化
- factory 注册新增 BashTool，test_factory 测试同步更新（3→4 tools）
- ChatRenderer 新增事件订阅不影响已有订阅

### 4.6 命名与文档 — ✅

- README.md 详尽（515 行），含设计决策、使用示例、安全考量
- Doxygen 注释完整
- 命名遵循项目约定

### 4.7 提交规范 — ⚠️

- commit message 遵循 Conventional Commits
- 无关联 Issue（`Closes #N` 缺失）

## ℹ️ Medium 发现

### M-1. 后台任务丢弃执行结果，LLM 无法获取输出

**位置：** [bash_tool.cpp#L348-L351](file:///d:/develop/Workspace/workx/src/agent/tool/BashTool/bash_tool.cpp#L348-L351)

**证据：**
```cpp
auto exec_result = process::exec(wrapped.cmd, opts);
// 后台任务的输出通过 TaskCompletedEvent 通知 UI
// 当前简化实现：丢弃输出，仅通知完成状态
// 后续迭代可扩展 Task 结构以保存输出，供 LLM 查询
(void)exec_result;
```

**影响：**
1. 后台任务完成后，LLM 无法获取命令输出（stdout/stderr/exit_code）
2. 如果 exec 返回 err（启动失败），TaskManager 仍标记为 Completed 而非 Failed
3. TaskCompletedEvent 不包含输出字段，ChatRenderer 只显示 "[bg] xxx completed"

**严重性：** Medium — commit message 明确标注为"简化实现"，但功能不完整可能导致 LLM 误以为命令成功。

**建议修复（合并后跟进）：**
```cpp
// 方案 A：扩展 TaskCompletedEvent 添加 output 字段
struct TaskCompletedEvent {
    // ...
    std::string output;  // 命令输出（可选）
};

// 方案 B：Task 结构保存输出，LLM 通过 tool_use 查询
// 方案 C（最小改动）：exec 失败时抛异常触发 markFailed
auto exec_result = process::exec(wrapped.cmd, opts);
if (exec_result.is_err()) {
    throw std::runtime_error(exec_result.error().message);
}
// 成功时保存输出到 task
```

### M-2. input_schema 未声明 cwd 字段，但 call() 读取它

**位置：** [bash_tool.cpp#L145-L176](file:///d:/develop/Workspace/workx/src/agent/tool/BashTool/bash_tool.cpp#L145-L176)（schema）, [bash_tool.cpp#L216-L219](file:///d:/develop/Workspace/workx/src/agent/tool/BashTool/bash_tool.cpp#L216-L219)（call）

**证据：** input_schema 声明了 command/description/timeout/run_in_background/dangerously_disable_sandbox，但未声明 `cwd`。call() 中：
```cpp
if (input.contains("cwd") && input["cwd"].is_string() && !input["cwd"].get<std::string>().empty()) {
    cwd = input["cwd"].get<std::string>();
}
```

由于 `additionalProperties: false`，LLM 传入 `cwd` 会被 schema 校验拒绝（取决于客户端实现）。

**影响：** LLM 无法通过参数指定工作目录，只能使用 ctx.cwd。功能上可接受（prompt 说"默认在当前工作目录执行"），但代码与 schema 不一致。

**建议：** 二选一：
- 方案 A：schema 添加 cwd 字段声明
- 方案 B：移除 call() 中的 cwd 读取（统一使用 ctx.cwd）

### M-3. 后台任务名截断可能产生重复

**位置：** [bash_tool.cpp#L332](file:///d:/develop/Workspace/workx/src/agent/tool/BashTool/bash_tool.cpp#L332)

**证据：**
```cpp
std::string task_name = "bash:" + command.substr(0, 40);
```

不同命令前 40 字符相同时产生相同 task_name。TaskManager 如果用 name 作为唯一标识可能冲突。

**影响：** 多个后台任务名重复，ChatRenderer 通知无法区分。

**建议：** 追加唯一标识：
```cpp
std::string task_name = "bash:" + command.substr(0, 40)
    + "#" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
```

## Low 发现

### L-1. strip_empty_lines 末尾多余换行

**位置：** [bash_tool.cpp#L59-L73](file:///d:/develop/Workspace/workx/src/agent/tool/BashTool/bash_tool.cpp#L59-L73)

**证据：** 每行都追加 `'\n'`，包括最后一行，导致输出末尾多一个换行。

**影响：** 输出格式轻微问题，不影响功能。

**建议：** 可接受现状；若优化可在循环后移除末尾换行。

### L-2. 无关联 Issue

同前几个 feature 分支，建议后续补 Issue。

## 动态验证结果

| 项 | 结果 |
|----|------|
| MSVC Debug 构建 | ✅ pass（无 warning） |
| 全量单元测试 | ✅ 587 cases / 2072 assertions 全通过 |
| `[bash]` 测试组 | ✅ 15 cases / 35 assertions 全通过 |
| `[factory]` 测试 | ✅ tool 数量 3→4 验证通过 |

## 跨代码库交叉验证

| 验证项 | 结果 |
|--------|------|
| BashTool 是否已注册到 factory | ✅ [factory.cpp#L167](file:///d:/develop/Workspace/workx/src/app/factory.cpp#L167) |
| TaskManager DI 是否注入到 ReActLoop | ✅ [chat_session.cpp#L240](file:///d:/develop/Workspace/workx/src/agent/core/chat_session.cpp#L240) |
| ChatRenderer 是否订阅 Task 事件 | ✅ [chat_renderer.cpp#L778-L802](file:///d:/develop/Workspace/workx/src/tui/render/chat_renderer.cpp#L778-L802) |
| test_factory 是否更新 | ✅ 3→4 tools |
| 沙盒依赖是否已合并到 develop | ✅ feat/sandbox 已合并（73fc1a3） |

## 合并建议

**✅ 建议合并**

**理由：**
1. BashTool 核心功能完整（同步执行/超时/取消/进度/沙盒），15 个测试覆盖全面
2. DI 集成完整（factory 注册 + ReActLoop 注入 TaskManager + ChatRenderer 事件订阅）
3. 不破坏已有代码（构造函数有默认值，测试同步更新）
4. 构建无警告，全量测试通过
5. 沙盒依赖已合并到 develop

**建议修复（可合并后跟进）：**
- M-1：后台任务输出保存与查询机制（当前为简化实现，commit message 已声明）
- M-2：schema 与 call() 的 cwd 字段一致性
- M-3：后台任务名唯一性

**可选修复：**
- L-1：strip_empty_lines 末尾换行
- L-2：补 Issue
