# AskUserTool — 向用户提问工具

> 通过 TUI 选择面板向用户提问，阻塞等待响应（含超时），支持单选/多选/自定义输入。
>
> 对标 Claude Code CLI 的 `AskUserQuestionTool`，在 WorkX 的 ReAct 架构下通过事件总线解耦工具线程与 TUI 主线程。
>
> 版本：v1.0.0

---

## 一、概述

`AskUserTool` 是 Agent 工具集中负责"向用户提问"的工具，对应 LLM 工具名 `AskUser`。支持：

- **阻塞式调用**：工具线程发布事件后阻塞等待用户响应，期间 TUI 主循环弹出 `ChoicePanel` 模态
- **超时控制**：默认 5 分钟，可通过 `timeout_ms` 自定义；`0` 表示不限时；超时自动返回 `timeout` 状态
- **多问题组合**：一次调用可提 1-4 个问题，每个问题独立 Tab
- **单选/多选**：通过 `multiSelect` 控制；单选空格互斥，多选空格独立勾选
- **自定义输入**：每个问题默认追加"✎ 自定义输入..."选项，用户可输入任意文本
- **事件驱动解耦**：工具线程仅发布 `AskUserRequestEvent`，TUI 主循环消费并回填 `promise`

### 文件清单

| 文件 | 用途 | 版本 |
|------|------|------|
| [AskUserTool.h](AskUserTool.h) | 工具接口声明 | v1.0.0 |
| [AskUserTool.cpp](AskUserTool.cpp) | 工具实现（输入校验 + 事件发布 + 阻塞等待 + 超时） | v1.0.0 |
| [README.md](README.md) | 本文档 | v1.0.0 |

### 依赖的内部模块

| 模块 | 用途 |
|------|------|
| [core/events/agent_events.h](../../../core/events/agent_events.h) | `AskUserRequestEvent` 事件结构 |
| [core/events/i_event_bus.h](../../../core/events/i_event_bus.h) | `IEventBus::publish_async()` 异步事件发布 |
| [agent/tool/context.h](../context.h) | `ToolContext::event_bus()` 事件总线访问 |
| [tui/widgets/choice_panel.h](../../../tui/widgets/choice_panel.h) | `ChoiceResult` / `parse_choice_config()` |
| [core/utils/result_v2.h](../../../core/utils/result_v2.h) | `ResultV2<ToolResult>` 返回类型 |

### 下游消费者（TUI 侧）

| 模块 | 职责 |
|------|------|
| [tui/render/chat_renderer.cpp](../../../tui/render/chat_renderer.cpp) | 订阅 `AskUserRequestEvent`，调用 `Terminal::set_pending_ask()` 唤醒主循环 |
| [tui/core/terminal.cpp](../../../tui/core/terminal.cpp) | 主循环检查 `woken_by_ask`，弹出 `run_choice_panel()`，结果回填 `promise` |
| [tui/core/platform/platform_win32.cpp](../../../tui/core/platform/platform_win32.cpp) | `notify_wake()` 通过 Windows Event 中断 `read_char()` 阻塞 |

---

## 二、工具元数据

| 字段 | 值 |
|------|-----|
| name | `AskUser` |
| description | Asks the user multiple choice questions to gather information, clarify ambiguity, understand preferences, make decisions or offer them choices. Blocks until the user responds or timeout (default 5 minutes) is reached. |
| namespace | `agent::tool` |
| 基类 | `ITool` |
| 同步/异步 | 同步返回 `ResultV2<ToolResult>`（内部阻塞等待 promise/future） |

---

## 三、输入 Schema

```json
{
  "type": "object",
  "properties": {
    "questions": {
      "type": "array",
      "minItems": 1,
      "maxItems": 4,
      "description": "Questions to ask the user (1-4 questions)",
      "items": {
        "type": "object",
        "properties": {
          "question":          { "type": "string",  "description": "完整问题文本（作为答案 map 的 key）" },
          "header":            { "type": "string",  "description": "短标签，Tab 栏显示名（≤12 字符）" },
          "multiSelect":       { "type": "boolean", "default": false, "description": "true=多选，false=单选（空格互斥）" },
          "allow_custom_input":{ "type": "boolean", "default": true,  "description": "是否追加 ✎ 自定义输入 选项" },
          "options": {
            "type": "array",
            "minItems": 2,
            "maxItems": 4,
            "description": "可选项（2-4 个）",
            "items": {
              "type": "object",
              "properties": {
                "label":       { "type": "string", "description": "显示文本（1-5 词）" },
                "description": { "type": "string", "description": "选项说明（可选）" }
              },
              "required": ["label"]
            }
          }
        },
        "required": ["question", "header", "options"]
      }
    },
    "timeout_ms": {
      "type": "integer",
      "default": 300000,
      "description": "超时毫秒数。0 = 不限时；默认 300000（5 分钟）。超时自动返回 timeout 状态。"
    }
  },
  "required": ["questions"]
}
```

### 字段说明

| 字段 | 类型 | 必填 | 默认值 | 说明 |
|------|------|------|--------|------|
| `questions` | array | 是 | — | 问题数组（1-4 个） |
| `questions[].question` | string | 是 | — | 完整问题文本，作为返回 answers 的 key |
| `questions[].header` | string | 是 | — | 短标签，Tab 栏显示名（≤12 字符） |
| `questions[].multiSelect` | bool | 否 | `false` | `true`=多选，`false`=单选（空格互斥） |
| `questions[].allow_custom_input` | bool | 否 | `true` | 是否在末尾追加"✎ 自定义输入..."选项 |
| `questions[].options` | array | 是 | — | 选项数组（2-4 个） |
| `questions[].options[].label` | string | 是 | — | 选项显示文本（1-5 词） |
| `questions[].options[].description` | string | 否 | — | 选项说明 |
| `timeout_ms` | int | 否 | `300000` (5min) | 超时毫秒；`0`=不限时；超时返回 `timeout` |

---

## 四、输出格式

### 4.1 用户提交（submitted）

```json
{
  "status": "submitted",
  "answers": {
    "选择哪种修复策略？": "重构",
    "选择要修改的文件？": "src/app/main.cpp,src/app/factory.cpp",
    "需要运行哪些验证？": "单元测试"
  }
}
```

- **单选**：answer 为选中项的 `label` 字符串
- **多选**：answer 为逗号分隔的 `label` 字符串
- **自定义输入**：answer 为用户输入的文本

### 4.2 用户取消（cancelled）

```json
{
  "status": "cancelled",
  "message": "User cancelled the question"
}
```

### 4.3 超时未响应（timeout）

```json
{
  "status": "timeout",
  "message": "User did not respond within 300s"
}
```

> 三种状态都通过 `ResultV2<ToolResult>::ok(ToolResult::ok(out))` 返回，不作为工具错误。让 LLM 根据 `status` 字段自主决策下一步动作。

---

## 五、执行管道

### 5.1 整体流程

```
LLM tool_call(AskUser, {questions, timeout_ms})
    │
    ▼
AskUserTool::call(input, ctx)
    │
    ├─ 1. 校验 input.questions 非空数组
    ├─ 2. 解析 timeout_ms（默认 300000）
    ├─ 3. parse_choice_config(input) 校验可被 ChoicePanel 解析
    ├─ 4. 创建 promise/future 通道
    ├─ 5. bus.publish_async(AskUserRequestEvent{questions, timeout_ms, promise})
    │      └─ 异步入队，立即返回
    ├─ 6. future.wait_for(timeout_ms)  ← 工作线程阻塞
    │      │
    │      │   ┌─── TUI 主循环（另一线程）──────────────────────┐
    │      │   │ ChatRenderer 订阅 AskUserRequestEvent 回调：    │
    │      │   │   → terminal.set_pending_ask(questions, promise)│
    │      │   │   → terminal.wake_main_loop()  ← notify_wake()  │
    │      │   │                                                 │
    │      │   │ 主循环 read_line() 被 KEY_WAKE 中断：            │
    │      │   │   → take_pending_ask()                          │
    │      │   │   → run_choice_panel(terminal, screen, config)  │
    │      │   │   → promise->set_value(choice)  ← 回填唤醒      │
    │      │   └─────────────────────────────────────────────────┘
    │      │
    │      ├─ status == ready ───→ future.get() 取出 ChoiceResult
    │      └─ status == timeout ─→ 构造 timeout 结果
    │
    └─ 7. 构造返回 JSON（submitted / cancelled / timeout）
           → ResultV2<ToolResult>::ok(ToolResult::ok(out))
```

### 5.2 跨线程协作时序

```
工作线程(ReActLoop)          事件总线           TUI 主循环          平台层
     │                          │                    │                  │
     │ publish_async(evt)       │                    │                  │
     ├─────────────────────────>│                    │                  │
     │                          │ enqueue            │                  │
     │                          ├───────────────────>│                  │
     │                          │  (ChatRenderer     │                  │
     │                          │   订阅回调)        │                  │
     │                          │                    │ set_pending_ask  │
     │                          │                    │ wake_main_loop   │
     │                          │                    ├─────────────────>│
     │                          │                    │                  │ SetEvent
     │                          │                    │                  │
     │ future.wait_for(...)     │                    │ read_char() 返回 │
     ├─ (阻塞)                  │                    │  KEY_WAKE        │
     │                          │                    │ take_pending_ask │
     │                          │                    │ run_choice_panel │
     │                          │                    │  (用户操作)      │
     │                          │                    │ promise.set_value│
     │                          │                    ├──────────────────┤
     │<─────────────────────────┼────────────────────┤                  │
     │ future.get() 就绪        │                    │                  │
     │                          │                    │                  │
```

### 5.3 超时判定逻辑

```cpp
if (timeout_ms > 0) {
    auto status = future.wait_for(std::chrono::milliseconds(timeout_ms));
    if (status == std::future_status::timeout) {
        timed_out = true;
    }
    // ready 或 timeout 都尝试 get：
    // - ready 时 get() 立即返回 ChoiceResult
    // - timeout 时若 TUI 恰好回填也能取到（竞态宽容）
} else {
    future.wait();  // 0 = 不限时
}
```

**关键设计**：
- 超时后不抛异常，构造 `timeout` 状态返回
- 即使 timeout，也允许 TUI 后续回填的 `ChoiceResult` 被丢弃（promise 析构时自动清理）
- `timeout_ms == 0` 时无限等待，适用于必须获得用户响应的关键决策

---

## 六、关键特性

### 6.1 事件驱动解耦

工具线程不直接访问终端 I/O（终端只能在主线程操作），而是通过事件总线发布请求：

| 组件 | 线程 | 职责 |
|------|------|------|
| `AskUserTool::call()` | ReActLoop 工作线程 | 校验输入、发布事件、阻塞等待 |
| `IEventBus::publish_async()` | 工作线程 | 异步入队，立即返回 |
| `ChatRenderer` 订阅回调 | 事件泵线程 | 设置 `pending_ask`，调用 `wake_main_loop()` |
| `Terminal::run_advanced()` | TUI 主线程 | 取出 `pending_ask`，弹出 `ChoicePanel`，回填 `promise` |
| `IPlatform::notify_wake()` | 事件泵线程 | Windows Event 信号，中断主线程的 `read_char()` |

### 6.2 跨线程唤醒机制

主循环通常阻塞在 `LineEditor::read_line()` → `IPlatform::read_char()`。唤醒通过两个步骤：

1. **设置 pending 请求**：`Terminal::set_pending_ask()` 加锁存入 `m_pending_ask`
2. **触发平台唤醒**：`IPlatform::notify_wake()` 设置 Windows Event

Windows 平台用 `WaitForMultipleObjects` 同时等待 stdin 和 wake_event：

```cpp
HANDLE handles[2] = { m_h_input, m_wake_event };
DWORD wait_result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
if (wait_result == WAIT_OBJECT_0 + 1) {
    ResetEvent(m_wake_event);
    return 0xE010;  // KEY_WAKE
}
```

主循环收到 `KEY_WAKE` 后返回 `ReadResult{.woken_by_ask = true}`，触发 ChoicePanel 弹出。

### 6.3 超时控制

| 常量 | 值 | 说明 |
|------|-----|------|
| `DEFAULT_TIMEOUT_MS` | `300000` (5min) | 默认超时，对齐 cc AskUserQuestionTool |
| `timeout_ms == 0` | 不限时 | 必须获得用户响应的关键决策 |
| `timeout_ms > 0` | 自定义超时 | 超时返回 `{"status": "timeout"}` |

超时后：
- 工作线程立即返回 `timeout` 结果给 LLM
- TUI 若仍在显示 ChoicePanel，用户后续操作回填的 `ChoiceResult` 会被 `future` 析构时丢弃
- LLM 收到 timeout 后可自主决策：重试、换方案、或终止任务

### 6.4 ChoicePanel 交互

弹出面板的完整交互由 [choice_panel.cpp](../../../tui/widgets/choice_panel.cpp) 实现：

| 按键 | 选择模式行为 | 输入模式行为 |
|------|-------------|-------------|
| ↑↓ | 移动光标；移到自定义项自动进入输入模式 | 先提交非空输入，再移动光标 |
| ←→ | 切换问题（环形） | 先提交非空输入，再切换 |
| 空格 | 勾选/取消（单选互斥）；自定义项进入输入模式 | — |
| Enter | 确认当前问题，跳到下一个；最后问题提交全部 | 确认输入并选中 |
| Esc | 取消整个面板 | 仅退出输入模式 |
| Backspace | — | UTF-8 安全删除最后一个码点 |
| 可打印字符 | — | 追加为 UTF-8 |

### 6.5 返回值映射

| ChoicePanel 结果 | AskUserTool 返回 |
|------------------|------------------|
| `submitted=true` | `{"status": "submitted", "answers": {question: answer}}` |
| `submitted=false`（Esc 取消） | `{"status": "cancelled", "message": "..."}` |
| `future.wait_for` 超时 | `{"status": "timeout", "message": "..."}` |

---

## 七、错误处理

所有错误通过 `ResultV2<ToolResult>::err()` 返回，不抛异常。

| 错误场景 | 错误码 | 错误信息 |
|---------|--------|---------|
| `questions` 缺失或非数组 | `InvalidInput` | `AskUser: missing 'questions' array` |
| `questions` 为空数组 | `InvalidInput` | `AskUser: 'questions' must not be empty` |
| `questions` 无法被 `parse_choice_config` 解析 | `InvalidInput` | `AskUser: failed to parse 'questions' as choice config` |
| `ctx.event_bus_ptr == nullptr` | `std::logic_error` | （由 `ctx.event_bus()` 抛出） |

> 用户取消和超时**不是**错误，作为 `ToolResult::ok` 返回，让 LLM 看到 `status` 字段后自主决策。

---

## 八、使用示例

### 8.1 单问题单选

```cpp
nlohmann::json input = {
    {"questions", nlohmann::json::array({
        nlohmann::json{
            {"question", "选择哪种修复策略？"},
            {"header", "策略"},
            {"multiSelect", false},
            {"options", nlohmann::json::array({
                nlohmann::json{{"label", "重构"}, {"description", "提取公共函数"}},
                nlohmann::json{{"label", "打补丁"}, {"description", "最小改动"}},
                nlohmann::json{{"label", "重写"}, {"description", "整体重写"}}
            })}
        }
    })}
};

auto r = tool.call(input, ctx);
// r.value().text 含:
// {"status":"submitted","answers":{"选择哪种修复策略？":"重构"}}
```

### 8.2 多问题混合 + 自定义超时

```cpp
nlohmann::json input = {
    {"timeout_ms", 60000},  // 1 分钟超时
    {"questions", nlohmann::json::array({
        nlohmann::json{
            {"question", "选择修复策略？"},
            {"header", "策略"},
            {"options", nlohmann::json::array({
                nlohmann::json{{"label", "重构"}},
                nlohmann::json{{"label", "打补丁"}}
            })}
        },
        nlohmann::json{
            {"question", "修改哪些文件？"},
            {"header", "文件"},
            {"multiSelect", true},
            {"options", nlohmann::json::array({
                nlohmann::json{{"label", "main.cpp"}},
                nlohmann::json{{"label", "factory.cpp"}}
            })}
        }
    })}
};

auto r = tool.call(input, ctx);
// 1 分钟内用户响应: {"status":"submitted","answers":{...}}
// 1 分钟超时: {"status":"timeout","message":"User did not respond within 60s"}
```

### 8.3 关闭自定义输入

```cpp
nlohmann::json input = {
    {"questions", nlohmann::json::array({
        nlohmann::json{
            {"question", "选择测试类型？"},
            {"header", "测试"},
            {"multiSelect", true},
            {"allow_custom_input", false},  // 不追加自定义输入项
            {"options", nlohmann::json::array({
                nlohmann::json{{"label", "单元测试"}},
                nlohmann::json{{"label", "集成测试"}}
            })}
        }
    })}
};
```

### 8.4 无限等待（关键决策）

```cpp
nlohmann::json input = {
    {"timeout_ms", 0},  // 不限时，必须获得用户响应
    {"questions", nlohmann::json::array({
        nlohmann::json{
            {"question", "是否执行破坏性操作？"},
            {"header", "确认"},
            {"options", nlohmann::json::array({
                nlohmann::json{{"label", "确认执行"}},
                nlohmann::json{{"label", "取消"}}
            })}
        }
    })}
};
```

---

## 九、与 Claude Code 对比

| 特性 | Claude Code `AskUserQuestionTool` | 本工具 (v1.0.0) | 差异说明 |
|------|----------------------------------|-----------------|----------|
| 工具名 | `AskUserQuestion` | `AskUser` | 简化命名 |
| 问题数 | 1-4 | 1-4 | 一致 |
| 选项数 | 2-4 | 2-4 | 一致 |
| multiSelect | ✅ | ✅ | 一致 |
| 自定义输入 | ✅（用户总能输入） | ✅（`allow_custom_input` 默认 true） | 本工具可显式关闭 |
| 选项 description | ✅ | ✅ | 一致 |
| header 短标签 | ✅ | ✅ | 一致 |
| 超时机制 | ❌（无限等待） | ✅（默认 5 分钟） | 本工具新增，防卡死 |
| 返回格式 | `{answers: {question: answer}}` | `{status, answers, message}` | 本工具多一个 `status` 字段 |
| 取消处理 | UI 层处理 | 工具层返回 `cancelled` | 本工具让 LLM 知道用户取消 |
| 跨线程实现 | Node.js 事件循环 | EventBus + promise/future + Windows Event | 机制不同 |
| TUI 渲染 | React 组件 | 自研 TUI `ChoicePanel` | 实现不同 |

### 设计差异分析

1. **超时机制**：cc 的 AskUserQuestion 无超时，本工具新增 `timeout_ms`（默认 5 分钟），避免工作线程永久阻塞导致 ReActLoop 卡死。超时后 LLM 收到 `timeout` 状态可自主决策重试或换方案。
2. **返回格式**：cc 直接返回 answers，本工具包裹 `status` 字段（submitted/cancelled/timeout），让 LLM 明确知道用户是否响应。
3. **跨线程协作**：cc 基于 Node.js 单线程事件循环，本工具是 C++ 多线程架构，需通过 EventBus + promise/future + 平台唤醒机制解耦工具线程与 TUI 主线程。
4. **自定义输入控制**：cc 的自定义输入始终可用，本工具增加 `allow_custom_input` 开关，对选项固定的场景（如"是/否"确认）可关闭以简化界面。

---

## 十、测试策略

单元测试见 [tests/unit/agent/tool/test_askuser_tool.cpp](../../../../tests/unit/agent/tool/test_askuser_tool.cpp)，覆盖：

| 类别 | 用例 | 期望结果 |
|------|------|---------|
| **元信息** | name / description / prompt 非空 | ✅ |
| **元信息** | schema 含 `questions` required + `timeout_ms` 可选 | ✅ |
| **参数校验** | 缺失 `questions` | err(InvalidInput) |
| **参数校验** | `questions` 为空数组 | err(InvalidInput) |
| **参数校验** | `questions` 非数组 | err(InvalidInput) |
| **参数校验** | question 缺失 `header` | err(InvalidInput) |
| **参数校验** | options 少于 2 个 | err(InvalidInput) |
| **超时** | `timeout_ms=100` + 不回填 promise | 返回 `{"status":"timeout"}` |
| **超时** | `timeout_ms=0` + 回填 promise | 返回 `{"status":"submitted"}` |
| **用户提交** | promise.set_value(submitted) | 返回 answers 映射 |
| **用户取消** | promise.set_value(cancelled) | 返回 `{"status":"cancelled"}` |
| **事件发布** | call 后 EventBus 有 AskUserRequestEvent | ✅ |

---

## 十一、设计决策

| 决策 | 理由 |
|------|------|
| 阻塞式 call() + promise/future | 遵循项目 `ResultV2<ToolResult>` 同步返回约定，与 BashTool/FileReadTool 一致 |
| 事件驱动而非直接调用 TUI | 终端 I/O 只能在主线程操作，工作线程通过事件解耦 |
| 默认超时 5 分钟 | 对齐 cc AskUserQuestion 的用户预期，避免永久卡死 |
| `timeout_ms == 0` 不限时 | 关键决策场景必须获得用户响应 |
| 超时/取消作为 ok 返回而非 err | 让 LLM 看到 status 自主决策，符合 cc 行为 |
| `allow_custom_input` 默认 true | 对齐 cc"用户总能输入"的体验 |
| `allow_custom_input` 可关闭 | 选项固定的场景（如确认对话框）可简化界面 |
| 用 `json::parse(R"JSON(...)JSON")` 而非初始化列表 | 避免 MSVC 编译深层嵌套 json 初始化列表时堆空间不足（C1060） |
| schema 中 `required` 用数组 `["label"]` | 原始字符串中无需转义引号，避免 MSVC 解析问题 |
| `AskUserRequestEvent` 携带 `shared_ptr<promise>` | 事件按值传递时 promise 仍共享同一实例 |
| `notify_wake()` 通过 Windows Event | 跨线程中断 `WaitForMultipleObjects` 阻塞，零开销 |
| 返回 answers 用 question 文本作为 key | LLM 可读性高，无需维护 question_id 映射 |

---

## 十二、路线图

### v1.0.0（已完成 ✅）

- [x] 单问题/多问题提问
- [x] 单选/多选（空格互斥/独立）
- [x] 自定义输入（自动进入输入模式）
- [x] 超时机制（默认 5 分钟，可自定义）
- [x] 事件驱动跨线程协作
- [x] TUI ChoicePanel 模态弹出
- [x] Windows 平台唤醒机制

### v1.1.0（短期）

- [ ] 单元测试补齐（参数校验 + 超时 + 事件发布）
- [ ] `timeout_ms` 上限钳制（如最大 30 分钟）
- [ ] 超时时向 TUI 发送取消信号（关闭仍在显示的 ChoicePanel）

### v1.2.0（中期）

- [ ] 问题类型扩展（文本输入、数字输入、日期选择）
- [ ] 问题依赖（前一个问题的答案影响后续问题选项）
- [ ] 默认选中项支持（`options[].default_selected`）

### v2.0.0（远期）

- [ ] 异步非阻塞模式（返回 question_id，后续通过事件接收答案）
- [ ] 多用户协作（多人同时回答同一问题）
- [ ] 问题持久化（会话恢复时重新弹出未回答的问题）
