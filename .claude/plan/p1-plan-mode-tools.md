# #28 实施计划：EnterPlanMode / ExitPlanMode 工具

日期：2026-08-13 ｜ 分支：feature/plan-mode-tools ｜ 基于 #36 权限体系

## 目标

AI 处理大型/不确定任务时主动进入计划模式（只读调研、输出方案），用户批准后才执行。
与 #36 的 `PermissionMode::Plan` 联动：进入计划模式 = 会话权限模式切 Plan（写/执行工具拒绝），批准后切回 Default。

## 设计

### 1. permission_mode 运行时注入路径（当前缺口）

`ToolContext::permission_mode` 无生产注入路径（react_loop.cpp:563 构造时未设置）。

- `ToolContext` 增加回调字段（CLAUDE.md「宿主能力 → 回调注入」模式）：
  ```cpp
  using PermissionModeChangedCallback = std::function<void(PermissionMode)>;
  PermissionModeChangedCallback on_permission_mode_changed = nullptr;
  void set_permission_mode(PermissionMode m);  // 非空回调则调用
  ```
- `ReActLoop` 增加成员 `PermissionMode m_permission_mode{Default}` + `set_permission_mode()`，
  构造 ToolContext 时绑定 `on_permission_mode_changed = [this](m){ m_permission_mode = m; }`

### 2. 事件（core/events/agent_events.h）

- `EnterPlanModeEvent`：{ session_id, reason } —— 宿主（TUI）收到后展示"计划模式"状态
- `ExitPlanModeEvent`：{ session_id, plan, approved } —— 展示方案与批准结果

### 3. 工具（agent/tool/PlanMode/）

| 工具 | 输入 | 行为 |
|---|---|---|
| EnterPlanModeTool | reason? | 发布 EnterPlanModeEvent；`ctx.set_permission_mode(Plan)`；返回提示 |
| ExitPlanModeV2Tool | plan（必填）| 复用 `ask_user_confirm` 展示方案 → 批准：set_permission_mode(Default) + 发布 ExitPlanModeEvent(approved=true)；拒绝：不退出但回执 approved=false |

- 已处于 Plan 时 Enter 幂等；非 Plan 时 Exit 且未批准 → 不切模式（fail-closed）
- exit 后若当前模式本来就是 Plan → 回 Default；若宿主已是 Bypass → 保持（不降级）

### 4. 注册

`src/app/factory.cpp register_builtin_tools()` 注册两个工具。

### 5. 测试（tests/unit/agent/tool/test_plan_mode_tools.cpp）

- ReActLoop.set_permission_mode → ToolContext 构造后 permission_mode 生效（FakeTool 读 ctx）
- EnterPlanModeTool：注入 fake 回调 → call 后回调收到 Plan；发布 EnterPlanModeEvent（FakeEventBus）
- ExitPlanModeV2Tool：无 event_bus → fail-closed 不批准；有总线且批准 → 回调收到 Default
- 联动：Plan 模式下 FileWriteTool check_permissions 拒绝（复用 #36 已验证逻辑）

## 验证

1. `cmake --preset default` + build + `[plan_mode]` 标签全绿
2. 全量 ctest 基线对比（15 失败为预存在）
