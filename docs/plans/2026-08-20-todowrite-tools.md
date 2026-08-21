# 内置 TodoWriteTool + TodoV2 系列规划

> 状态：已实现（2026-08-20）
> 范围：`src/agent/tool`、`src/core`、`src/ftxtui`、`src/agent/session`
> 基线：`docs/plans/2026-08-17-ftxui-tui-design.md`（事件→Action→ViewModel 链路）
> 关联：GitHub Issue #24「[内置工具] 缺失 TodoWriteTool：复杂任务的进度跟踪能力」（P0，milestone 0.5.x 工具系统补齐）

---

## 0. 决策记录

| 决策点 | 结论 | 说明 |
| --- | --- | --- |
| 工具范围 | **TodoWrite + TodoV2 系列（TaskCreate/Get/Update/List）** | 用户确认两档都做；TodoWrite 全量快照风格，Task 系列细粒度 CRUD |
| 数据存储 | **单例 TodoStore（mutex，按 session 分桶）+ 事件桥接** | 用户确认；对齐 FileHistory / FileReadStateTracker 单例范式 |
| UI 展示 | **侧边栏状态图标 + StatusBar 进度位** | 用户确认；collapsible TODO 区升级为 ✓/▶/○ 彩色行，状态栏加 `✓ 2/5` |
| 持久化 | **会话 JSONL 持久化** | 用户确认；SessionStore 新增 `todo` 事件，/resume 时恢复 |
| TodoItem 类型归属 | **core 层规范类型** | 对齐 `core/tool_kind.h` 的 C-3 模式：core 放纯数据结构，agent 层引用，避免 core→agent 分层越界 |
| V1/V2 互斥 | **都注册，prompt 引导** | C++ 端无 feature flag 基础设施；模型按任务复杂度自行选择（简单清单用 TodoWrite，细粒度任务用 Task 系列） |
| 事件载荷 | **完整快照** | 每次变更发布完整 todo 列表，UI 侧直接替换，避免增量同步的复杂度 |
| 持久化接线 | **TodoStore 单例持 session 级持久化回调** | ChatSession 注册回调写入 SessionStore；switch_session 时重新接线并恢复内存态 |

---

## 1. 目标与范围

### 1.1 目标

1. 新增内置 `TodoWrite` 工具：AI 主动创建/更新会话待办清单（全量替换语义，对齐 Claude Code）。
2. 新增 `TaskCreate / TaskGet / TaskUpdate / TaskList` 四个细粒度任务管理工具（对齐 cc TodoV2 系列）。
3. 工具更新后经事件总线推送到 TUI：侧边栏 TODO 区显示带状态图标的清单，StatusBar 显示 `✓ 完成数/总数` 进度。
4. 待办清单持久化到会话 JSONL（`todo` 事件），`/resume` 恢复历史会话时自动恢复待办状态，支持任务中断后继续。

### 1.2 非目标（本期不做）

- 多 Agent 协作（owner / blocks / blockedBy 字段保留在数据结构中，但本期不实现跨 Agent 通知、mailbox、claimTask 等 swarm 能力）。
- 验证 Agent（verification nudge）——cc 的 `verificationNudgeNeeded` 逻辑依赖 VERIFICATION_AGENT feature flag，C++ 端无此体系，本期不实现。
- 任务依赖图可视化（blocks/blockedBy 仅存储与展示，不做拓扑排序）。
- 独立"任务"tab —— 待办展示复用现有侧边栏 TODO 区 + 任务调度 tab，不新增 tab。

---

## 2. 核心设计

### 2.1 TodoItem 规范类型（core 层）

**新文件：`src/core/todo/todo_item.h`**（纯头文件，无依赖）

```cpp
namespace core::todo {

/// @brief 待办状态（对齐 cc TaskStatusSchema）
enum class TodoStatus : uint8_t { Pending = 0, InProgress, Completed };

/// @brief 待办条目（core 层规范类型，agent/ftxtui/events 共用）
struct TodoItem {
    std::string id;              ///< TaskV2: "1","2",...；TodoWrite: 空
    std::string content;         ///< 命令式措辞（subject），如 "Run tests"
    std::string active_form;     ///< 进行式措辞，如 "Running tests"（in_progress 时显示）
    TodoStatus status{TodoStatus::Pending};
    std::string description;     ///< TaskV2 可选
    std::string owner;           ///< TaskV2 可选（本期不用于协作）
    std::vector<std::string> blocks;     ///< 本任务阻塞的任务 id
    std::vector<std::string> blocked_by; ///< 阻塞本任务的任务 id
    nlohmann::json metadata;     ///< 任意元数据

    /// @brief 状态 → 字符串（"pending"/"in_progress"/"completed"）
    static const char* status_str(TodoStatus s);
    /// @brief 字符串 → 状态（非法返回 Pending）
    static TodoStatus status_from(const std::string& s);
};

} // namespace core::todo
```

**CMake**：`src/core/CMakeLists.txt` 的 `target_sources` 增加头文件（纯头文件，仅 IDE 导航用）。

### 2.2 TodoStore 单例（agent::tool 层）

**新文件：`src/agent/tool/TodoStore/todo_store.h` + `todo_store.cpp`**

```cpp
namespace agent::tool {

/// @brief TodoStore — 待办清单单例（按 session 分桶，mutex 保护）
/// @details 对齐 FileHistory 单例范式：工具 call() 为 const 方法、跨线程并行，
///          内部用 std::mutex 保护。变更后发布 TodoUpdatedEvent（经 IEventBus）。
class TodoStore {
public:
    static TodoStore& instance();

    // ---- CRUD（V2 细粒度）----
    std::string create_todo(const std::string& session_id, const TodoItem& item); // 返回自增 id
    std::optional<TodoItem> get_todo(const std::string& session_id, const std::string& id) const;
    bool update_todo(const std::string& session_id, const std::string& id,
                     const std::function<void(TodoItem&)>& mutate);
    bool delete_todo(const std::string& session_id, const std::string& id);
    std::vector<TodoItem> list_todos(const std::string& session_id) const;

    // ---- 全量替换（V1 TodoWrite）----
    std::vector<TodoItem> replace_todos(const std::string& session_id,
                                        const std::vector<TodoItem>& todos);

    // ---- 持久化接线 ----
    /// @brief 注册 session 级持久化回调（ChatSession 写入 SessionStore）
    void set_persist_callback(const std::string& session_id,
                              std::function<void(const std::vector<TodoItem>&)> cb);
    /// @brief 恢复内存态（/resume 时 ChatSession 调用）
    void restore_todos(const std::string& session_id, std::vector<TodoItem> todos);

private:
    struct SessionState {
        std::vector<TodoItem> todos;
        int next_id = 1;                     ///< 自增 id（对齐 cc high water mark）
        std::function<void(const std::vector<TodoItem>&)> persist_cb;
    };
    mutable std::mutex m_mutex;
    std::unordered_map<std::string, SessionState> m_sessions;
};

} // namespace agent::tool
```

**关键语义**：
- `create_todo`：分配 `std::to_string(next_id++)`，插入后发布事件 + 调持久化回调。
- `replace_todos`：TodoWrite 全量替换；若传入列表全部 completed，则置空（对齐 cc `allDone ? [] : todos`）。
- `update_todo`：按 id 找到条目，调用 mutate 修改（支持 status/subject/description/activeForm/blocks 等），`status == "deleted"` 时删除。
- 每次变更：持锁修改 → 解锁 → `event_bus().publish_async(TodoUpdatedEvent{...})` → 调持久化回调。
- 事件发布与持久化回调在**锁外**执行，避免回调中再次获取锁导致死锁（对齐项目"事件发布必须在锁释放后"的既有约束）。

**事件总线来源**：TodoStore 单例需要 IEventBus。对齐 FileHistory 的注入方式——由 ChatSession 在构造时 `TodoStore::instance().set_event_bus(&bus)` 注入（非拥有指针），未注入时跳过发布（工具仅返回结果）。

### 2.3 工具实现

#### 2.3.1 TodoWriteTool（V1 全量快照）

**新文件：`src/agent/tool/TodoWriteTool/todo_write_tool.h` + `.cpp`**

- `name()` = `"TodoWrite"`
- `is_read_only()` = `false`
- `input_schema()`（对齐 cc TodoWriteTool 输入）：
```json
{
  "type": "object",
  "properties": {
    "todos": {
      "type": "array",
      "items": {
        "type": "object",
        "properties": {
          "content":    {"type": "string", "description": "Imperative form, e.g. 'Run tests'"},
          "status":     {"type": "string", "enum": ["pending", "in_progress", "completed"]},
          "activeForm": {"type": "string", "description": "Present continuous form, e.g. 'Running tests'"}
        },
        "required": ["content", "status", "activeForm"]
      }
    }
  },
  "required": ["todos"]
}
```
- `call()` 逻辑：
  1. 解析 `todos` 数组 → `vector<TodoItem>`（id 置空）。
  2. `old = TodoStore::instance().list_todos(session_id)`。
  3. `new = TodoStore::instance().replace_todos(session_id, parsed)`（全部 completed 时置空）。
  4. 返回 `ToolResult::ok(Json)`：`{"oldTodos": [...], "newTodos": [...]}`。
- `prompt()`：对齐 cc `prompt.ts` 的完整指导（何时用/何时不用/状态管理/两种措辞），精简为 C++ 字符串。

#### 2.3.2 TaskCreateTool

**新文件：`src/agent/tool/TaskTools/task_create_tool.h` + `.cpp`**

- `name()` = `"TaskCreate"`
- `input_schema()`：`{subject(必), description(选), activeForm(选), metadata(选)}`
- `call()`：`create_todo` 创建 `status=pending` 的任务，返回 `{"task": {"id": ..., "subject": ...}}`。
- 返回文本：`Task #<id> created successfully: <subject>`。

#### 2.3.3 TaskGetTool

- `name()` = `"TaskGet"`
- `input_schema()`：`{taskId(必)}`
- `call()`：`get_todo`，存在返回任务对象，不存在返回 `null`（非错误，对齐 cc）。
- 返回文本：`#<id> [<status>] <subject>` 或 `Task #<id> not found`。

#### 2.3.4 TaskUpdateTool

- `name()` = `"TaskUpdate"`
- `input_schema()`：`{taskId(必), subject?, description?, activeForm?, status?(含 "deleted"), addBlocks?, addBlockedBy?, owner?, metadata?}`
- `call()`：
  1. 任务不存在 → `{"success": false, "error": "Task not found"}`（非错误返回）。
  2. `status == "deleted"` → 删除任务，返回 `{"success": true, "updatedFields": ["deleted"]}`。
  3. 其余字段按需更新，记录 `updatedFields` 数组。
  4. `addBlocks` / `addBlockedBy`：追加到 blocks / blocked_by（去重）。
  5. 返回 `{"success": true, "taskId", "updatedFields", "statusChange": {from, to}}`。
- 返回文本：`Updated task #<id> <fields...>`。

#### 2.3.5 TaskListTool

- `name()` = `"TaskList"`
- `is_read_only()` = `true`
- `input_schema()`：`{}`
- `call()`：`list_todos`，过滤 `metadata._internal` 为 true 的条目，返回 `{"tasks": [{id, subject, status, owner?, blockedBy}]}`。
- 返回文本：每行 `#<id> [<status>] <subject>`，空列表返回 `No tasks found`。

### 2.4 事件与 Action 链路

#### 2.4.1 TodoUpdatedEvent（core 层）

**修改：`src/core/events/agent_events.h`**

```cpp
/// @brief 待办更新事件（TodoStore → TUI，携带完整快照）
struct TodoUpdatedEvent {
    std::string session_id;
    std::vector<core::todo::TodoItem> todos;  ///< 完整快照（UI 直接替换）
};
```

（`agent_events.h` 已 include `core/tool_kind.h`，同样 include `core/todo/todo_item.h` 即可。）

#### 2.4.2 ActionTodoUpdate（ftxtui 层）

**修改：`src/ftxtui/bridge/action.h`**

```cpp
/// @brief 待办更新（TodoUpdatedEvent → UI 线程）
struct ActionTodoUpdate {
    std::vector<core::todo::TodoItem> todos;  ///< 完整快照
};
```

加入 `Action` variant。

#### 2.4.3 EventBridge 订阅

**修改：`src/ftxtui/bridge/event_bridge.cpp`**

```cpp
subscribe_typed<agent::TodoUpdatedEvent>(
    [this](const agent::TodoUpdatedEvent& e) {
        push(ActionTodoUpdate{.todos = e.todos});
    });
```

#### 2.4.4 ViewModel 处理

**修改：`src/ftxtui/vm/view_model.h` + `.cpp`**

- `SidebarModel.todos` 从 `std::vector<std::string>` 升级为 `std::vector<core::todo::TodoItem>`。
- 新增 `apply_variant(const ActionTodoUpdate&)`：`sidebar.todos = a.todos;` 返回 true（触发重绘）。

### 2.5 UI 渲染

#### 2.5.1 侧边栏 TODO 区（状态图标）

**修改：`src/ftxtui/widgets/sidebar.cpp`**

`append_collapsible_section` 目前渲染 `vector<string>`。为 TODO 区新增专用渲染（保留通用函数给 MCP 区用）：

```cpp
/// @brief TODO 区：✓(completed 绿) / ▶(in_progress 蓝) / ○(pending 灰) + content
void append_todo_section(Elements& rows, const std::vector<core::todo::TodoItem>& todos,
                         bool expanded, SectionHit* hit);
```

- 标题行：`▾ TODO`（复用 `str::kSidebarTODO`，可折叠，点击切换 `todo_expanded`）。
- 每行：`  ✓  Run tests` / `  ▶  Running tests`（in_progress 显示 activeForm）/ `  ○  Fix bug`。
- 颜色：completed=绿、in_progress=主题 Accent（蓝）、pending=TextDim。
- 空列表显示 `—`（复用 `str::kDash`）。
- `append_sidebar_info` 中 TODO 区改调 `append_todo_section`。

#### 2.5.2 StatusBar 进度位

**修改：`src/ftxtui/widgets/status_line.h` + `.cpp`**

- `build_status_line()` 增加参数 `const std::vector<core::todo::TodoItem>& todos`（或 `int done, int total`）。
- 有待办时，在权限模式与模型之间插入片段：`✓ 2/5`（completed/total，绿色），无待办不显示。
- **修改：`src/ftxtui/app.cpp`** 调用处传入 `m_vm.sidebar.todos`。

### 2.6 JSONL 持久化

#### 2.6.1 SessionStore 新增 todo 事件

**修改：`src/agent/session/session_store.h` + `.cpp`**

```cpp
/// @brief 追加 todo 事件（TodoStore 变更后写入）
bool append_todo(const std::vector<core::todo::TodoItem>& todos);

/// @brief 读取 JSONL 中最后一条 todo 事件（无则返回空）
static std::vector<core::todo::TodoItem> load_todos(const std::string& file_path);
```

- 事件格式：`{"type": "todo", "todos": [...], "timestamp": "..."}`。
- `load_messages` 过滤 `type == "todo"` 的事件（不作为消息加载）。
- `load_todos` 取最后一条 todo 事件反序列化。

#### 2.6.2 ChatSession 接线

**修改：`src/agent/core/chat_session.cpp`**

1. 构造时：`TodoStore::instance().set_event_bus(&m_event_bus.get())`。
2. `configure_session_store` / `switch_session` 成功后：
   - 注册持久化回调：`TodoStore::instance().set_persist_callback(session_id, [store](todos){ store->append_todo(todos); })`。
   - `switch_session` 时：`restore_todos(session_id, SessionStore::load_todos(file_path))` 恢复内存态，并发布一次 `TodoUpdatedEvent` 刷新 UI。
3. `clear_history` 时：清空该 session 的 TodoStore 状态（`TodoStore::instance().clear_session(session_id)`，需在 TodoStore 增加该方法）。

### 2.7 注册与构建

**修改：`src/agent/factory.cpp`** — `register_builtin_tools()` 增加：

```cpp
registry.register_tool(std::make_shared<tool::TodoWriteTool>());
registry.register_tool(std::make_shared<tool::TaskCreateTool>());
registry.register_tool(std::make_shared<tool::TaskGetTool>());
registry.register_tool(std::make_shared<tool::TaskUpdateTool>());
registry.register_tool(std::make_shared<tool::TaskListTool>());
```

**修改：`src/agent/CMakeLists.txt`** — `target_sources` 增加：

```cmake
tool/TodoStore/todo_store.cpp
tool/TodoWriteTool/todo_write_tool.cpp
tool/TaskTools/task_create_tool.cpp
tool/TaskTools/task_get_tool.cpp
tool/TaskTools/task_update_tool.cpp
tool/TaskTools/task_list_tool.cpp
```

（`src/core/CMakeLists.txt` 增加 `todo/todo_item.h` 头文件条目。）

---

## 3. 测试计划

新增测试文件（`tests/unit/`，沿用 GLOB_RECURSE 自动收集）：

| 文件 | 覆盖 |
| --- | --- |
| `tests/unit/core/todo/test_todo_item.cpp` | TodoItem 状态字符串转换、JSON 序列化往返 |
| `tests/unit/agent/tool/test_todo_store.cpp` | create/get/update/delete/list/replace、自增 id、全部 completed 置空、并发安全（多线程 create）、事件发布计数（MockEventBus） |
| `tests/unit/agent/tool/test_todo_tools.cpp` | 5 个工具的 schema 校验、call 逻辑（含 TaskUpdate deleted、TaskList 过滤 _internal、TaskGet 不存在返回 null）、错误输入返回 |
| `tests/unit/agent/session/test_session_store_todo.cpp` | append_todo / load_todos 往返、load_messages 过滤 todo 事件 |
| `tests/unit/ftxtui/test_view_model_todo.cpp` | ActionTodoUpdate 更新 SidebarModel.todos |

复用既有测试基建：`MockEventBus`（published_count 断言）、`MockConfigManager`、`fill_ctx` 模式。

---

## 4. 实施步骤（分阶段）

### Phase 1：core 层基础
1. 新建 `src/core/todo/todo_item.h`（TodoItem + TodoStatus）。
2. `src/core/CMakeLists.txt` 增加头文件条目。

### Phase 2：TodoStore 单例
3. 新建 `src/agent/tool/TodoStore/todo_store.h/.cpp`（含事件发布 + 持久化回调 + clear_session）。
4. `src/agent/CMakeLists.txt` 增加 todo_store.cpp。

### Phase 3：五个工具
5. 新建 `TodoWriteTool/`、`TaskTools/` 五个工具（h/cpp）。
6. `factory.cpp` 注册 + `src/agent/CMakeLists.txt` 增加源文件。

### Phase 4：事件 → Action → UI
7. `agent_events.h` 增加 `TodoUpdatedEvent`。
8. `action.h` 增加 `ActionTodoUpdate`；`event_bridge.cpp` 订阅。
9. `view_model.h/.cpp`：`SidebarModel.todos` 升级类型 + `apply_variant(ActionTodoUpdate)`。
10. `sidebar.cpp`：TODO 区状态图标渲染。
11. `status_line.h/.cpp` + `app.cpp`：StatusBar 进度位。

### Phase 5：JSONL 持久化
12. `session_store.h/.cpp`：`append_todo` / `load_todos` / `load_messages` 过滤。
13. `chat_session.cpp`：TodoStore 事件总线注入、持久化回调接线、switch_session 恢复、clear_history 清理。

### Phase 6：测试
14. 按 §3 编写 5 个测试文件。
15. 构建 + `ctest` 全量通过（含既有 8 个 [slow] 用例）。

---

## 5. 风险与注意

- **并发安全**：TodoStore 单例被工具 call() 跨线程访问，所有读写必须持锁；事件发布/持久化回调在锁外执行（对齐项目既有约束）。
- **SidebarModel.todos 类型变更**：需同步检查所有引用点（sidebar.cpp 渲染、view_model.cpp），避免编译遗漏。
- **MSVC 容器搬移**：TodoStore 内部 `unordered_map<string, SessionState>` 的 value 含 `std::function`，扩容搬移安全（无地址稳定需求）；若后续持有元素引用需用 unique_ptr（对齐项目记忆中的 deque 教训）。
- **switch_session 时序**：恢复 TodoStore 内存态 + 发布事件必须在切换完成、UI 可重绘后执行，避免竞态。
- **schema 稳定性**：工具 schema 在 build_request 时按 name 排序发送（既有约束），新增工具自动纳入，不影响缓存前缀稳定性。

---

## 6. 验收标准

1. 系统提示词包含 5 个工具的 prompt；LLM 可调用 TodoWrite 创建清单。
2. 工具调用后侧边栏 TODO 区实时显示带状态图标的清单。
3. StatusBar 显示 `✓ 完成数/总数` 进度。
4. `/resume` 恢复历史会话后，待办清单与状态完整恢复。
5. 全部单元测试通过；无新增 [slow] 用例。
