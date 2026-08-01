# 会话恢复 /resume 与 /rename 设计

> 基于 `feat/session-restore` 分支的 SessionStore 基础设施，新增 TUI 内 `/resume`（切换会话）和 `/rename`（修改标题）命令。

## 设计决策

| 决策点 | 选择 |
|--------|------|
| 标题存储 | 新增 `title` 事件类型（append-only，兼容旧文件） |
| 标题来源 | 首条 user 消息前 20 字自动生成；`/rename` 追加新 title 事件覆盖 |
| 切换写入语义 | 追加到历史会话文件（当前会话已实时持久化，不丢失） |
| session_end | 切换时不写 session_end（会话可被多次 resume 继续） |

## 数据模型

### 新增 title 事件

```jsonl
{"type":"title","uuid":"...","parentUuid":"...","timestamp":"...","title":"重构压缩器架构"}
```

### 读取规则

- `load_meta`/`list_sessions` 扫描所有 `title` 事件，取最后一条作为当前标题
- 无 `title` 事件：fallback 首条 user 消息前 20 字
- 都没有：`"未命名会话"`

### SessionMeta 扩展

```cpp
struct SessionMeta {
    // 现有字段...
    std::string title;  // 新增
};
```

### 标题生成时机

首条 user 消息持久化时，自动追加 `title` 事件（前 20 字）。

### /rename 持久化

`SessionStore::append_title(name)` 追加新 `title` 事件到当前会话文件。

## ChatSession 切换会话

```cpp
bool switch_session(const std::string& file_path);
```

**流程**（单一锁作用域）：
1. `load_messages` + `load_meta` 加载历史
2. 替换 `m_session_id`（从文件名 stem 提取）
3. 清空 `m_messages`，填入历史消息
4. 关闭旧 SessionStore，创建新 SessionStore 指向历史文件，append 模式打开
5. 不追加 session_start（会话进行中）
6. 重置压缩器和前缀形状基线

**不写 session_end**：会话可被多次 resume 继续。

## 命令

### /resume

- 打开 TUI 会话选择面板（搜索框 + 会话标题列表）
- 上下键选择，Enter 确认，Esc 取消
- 搜索框实时过滤（按标题/时间/分支匹配）
- 确认后调用 `session->switch_session(file_path)`

### /rename \<name\>

- 修改当前会话标题
- 调用 `session_store->append_title(name)`
- 无参数时提示输入

## CommandContext 扩展

为让命令访问 ChatSession，`CommandContext` 增加 `ChatSession* session` 字段。

## TUI 会话选择面板

参考 `FileSearchPanel` 实现，复用自研 TUI 框架：
- 搜索框（顶部，实时输入过滤）
- 会话列表（标题 + 时间 + 分支 + 消息数）
- 上下键选择，Enter 确认，Esc 取消
- 最多显示 7 条，滚动窗口

## 文件清单

- 修改：`src/agent/session/session_store.h/.cpp`（title 事件 + SessionMeta.title）
- 修改：`src/agent/core/chat_session.h/.cpp`（switch_session + session_id setter + persist 自动生成标题）
- 修改：`src/agent/command/inclaude/types.h`（CommandContext 加 session 指针）
- 修改：`src/app/command/builtin_commands.cpp`（注册 /resume /rename）
- 新增：`src/tui/widgets/session_picker.h/.cpp`（会话选择面板）
- 修改：`src/app/main.cpp`（接线 CommandContext + 命令注册）
- 新增测试：`tests/unit/agent/session/test_session_title.cpp`

## 验收标准

- [ ] SessionStore 支持 title 事件读写
- [ ] load_meta/list_sessions 返回 title（含 fallback 逻辑）
- [ ] ChatSession.switch_session 切换会话文件并继续追加
- [ ] /resume 打开 TUI 面板，搜索过滤 + 上下选择 + Enter 切换
- [ ] /rename 修改当前会话标题，/resume 列表显示新标题
- [ ] 首条 user 消息自动生成标题（前 20 字）
- [ ] 全量测试通过
