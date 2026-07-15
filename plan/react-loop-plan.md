# ReAct 循环实现计划

> 替换 ChatSession 中的 while 单向循环，实现 Thought → Action → Observation 显式分离
> 日期：2026-07-16

---

## 一、现状分析

### 当前实现（chat_session.cpp run_completion）

```
run_completion(user_text)
  └─ TaskManager.launch("completion", lambda)
       └─ while (iteration < 25)          ← 扁平 while 循环
            ├─ build_request()             ← 构建 CompletionRequest
            ├─ provider->submit_completion()
            ├─ while (true)                ← 流式读取内循环
            │    ├─ reader->next(chunk)
            │    ├─ HasData → 累积 content/reasoning/tool_use
            │    ├─ Complete → break
            │    ├─ Error → break
            │    └─ Cancelled → break
            ├─ if (stream_error) → retry / return
            ├─ if (stream_cancelled) → return
            ├─ if (pending_tools.empty()) → 最终回复，return
            ├─ 构建 assistant 消息（含 tool_uses）
            ├─ for each tool_use:
            │    ├─ execute(tool_name, input)
            │    ├─ publish StreamTokenEvent ([Tool: name])
            │    └─ add tool_result 消息
            └─ continue                   ← 下一轮
```

### 问题清单

| 问题 | 现状 | 目标 |
|------|------|------|
| 无显式阶段分离 | while 循环混合 Thought/Action/Observation | Thought → Action → Observation 三阶段显式建模 |
| 无步骤追踪 | 仅靠 messages 隐式记录 | ReActStep 结构化记录每一步 |
| Agent 事件未使用 | AgentStepEvent/ToolCallEvent/ToolResultEvent/AgentDoneEvent 已定义但未发布 | 循环中发布结构化事件 |
| 循环逻辑与会话管理耦合 | ChatSession 同时处理 retry/persist/agent loop | ChatSession 管理会话，ReActLoop 管循环 |
| 无可观测性 | UI 只看到 token 流 | UI 可展示 Thought/Action/Observation 步骤 |
| react_loop.h 是空 stub | 仅注释 | 完整类实现 |

### 已有基础设施

```
src/agent/core/
├── chat_session.h/cpp      ← 当前 while 循环（将被重构）
├── react_loop.h            ← 空 stub（将被实现）
└── query_engine.h          ← 空 stub（本计划不涉及）

src/agent/message/types.h   ← Agent 事件已定义但未使用：
  ├─ AgentStepEvent         { step_id, step_number, description }
  ├─ ToolCallEvent          { tool_name, arguments, call_id, tool_type }
  ├─ ToolResultEvent        { call_id, result, is_error }
  └─ AgentDoneEvent         { final_response, total_steps, total_tool_calls, total_duration_ms }
```

---

## 二、目标设计

### ReAct 模式映射

```
┌─────────────────────────────────────────────────┐
│                ReAct 循环                        │
│                                                  │
│  ┌──────────┐     ┌──────────┐     ┌─────────┐ │
│  │ Thought  │────→│ Action   │────→│ Observe │ │
│  │ LLM 推理  │     │ 工具调用  │     │ 结果回传 │ │
│  └──────────┘     └──────────┘     └─────────┘ │
│       ↑                                  │       │
│       └──────────────────────────────────┘       │
│                                                  │
│  终止条件: 无 tool_use / max_iterations / cancel │
└─────────────────────────────────────────────────┘
```

| ReAct 阶段 | 实现 | 数据来源 | 发布事件 |
|-----------|------|---------|---------|
| **Thought** | 流式读取 LLM 响应 | content + reasoning + tool_uses | StreamTokenEvent, AgentStepEvent |
| **Action** | 执行工具调用 | tool_use → ToolExecutor | ToolCallEvent, StreamTokenEvent |
| **Observation** | 结果注入对话历史 | tool_result 消息 | ToolResultEvent, StreamTokenEvent |
| **FinalAnswer** | 无 tool_use 时终止 | assistant 消息 | StreamDoneEvent, AgentDoneEvent |

### 核心原则

1. **原生 function calling**：不使用文本解析 "Thought:" / "Action:"，直接使用 Anthropic/OpenAI 的 tool_use 机制
2. **Thought = LLM 文本输出**：content + reasoning_content 即为 LLM 的思考过程
3. **Action = tool_use 块**：LLM 生成的工具调用请求
4. **Observation = tool_result**：工具执行结果回传给 LLM

---

## 三、架构设计

### 类关系

```
ChatSession（会话管理）
  ├─ 持有 ICompletionProvider
  ├─ 持有 ToolRegistry
  ├─ 持有 ReActLoop
  ├─ 负责: retry / persist / interrupt / 状态管理
  │
  └─ run_completion()
       └─ ReActLoop::run(messages, system_prompt, should_cancel)
            ├─ Thought: ICompletionProvider → IStreamReader → StreamChunk
            ├─ Action:  ToolExecutor → ExecutionResult
            └─ Observation: messages.push_back(tool_result)
```

### ReActLoop 类设计

```cpp
// react_loop.h

namespace agent {

/// @brief ReAct 步骤类型
enum class ReActStepType {
    Thought,        ///< LLM 推理（流式输出文本 + 工具调用决策）
    Action,         ///< 工具调用执行
    Observation,    ///< 工具结果回传
    FinalAnswer     ///< 最终回复（终止）
};

/// @brief 单个 ReAct 步骤记录
struct ReActStep {
    ReActStepType type;
    int step_number = 0;               // 步骤序号（从 1 开始）

    // Thought 阶段
    std::string thought_text;          // LLM 文本输出
    std::string reasoning;             // LLM 推理内容（deepseek thinking 等）
    std::vector<ToolUse> tool_uses;    // LLM 决定调用的工具

    // Action 阶段
    std::string tool_name;             // 当前执行的工具名
    nlohmann::json tool_input;         // 工具输入

    // Observation 阶段
    std::string observation;           // 工具结果文本
    bool is_error = false;             // 工具执行是否出错

    // 元信息
    double duration_ms = 0.0;          // 本步骤耗时
};

/// @brief ReAct 循环执行结果
struct ReActResult {
    // 步骤历史
    std::vector<ReActStep> steps;

    // 最终输出
    std::string final_answer;
    std::string final_reasoning;

    // 统计信息
    int total_iterations = 0;          // 总迭代轮数
    int total_tool_calls = 0;          // 总工具调用次数
    double total_duration_ms = 0.0;    // 总耗时

    // token 统计（最后一次 LLM 响应的）
    int32_t prompt_tokens = 0;
    int32_t generated_tokens = 0;

    // 状态
    bool was_interrupted = false;      // 用户中断
    bool was_error = false;            // 发生错误
    std::string error_message;         // 错误信息
};

/// @brief ReActLoop — Reason → Act → Observe 迭代循环
///
/// 从 ChatSession 中提取的 agent 循环逻辑：
/// - Thought: 向 LLM 发送请求，流式获取推理与工具调用决策
/// - Action: 执行工具调用
/// - Observation: 将工具结果注入对话历史
/// - 重复直至 LLM 给出最终回复（无工具调用）或达到终止条件
class ReActLoop {
public:
    /// @brief 循环配置
    struct Config {
        int max_iterations = 25;       ///< 最大迭代轮数
        // 未来扩展: max_tokens, timeout, compact_threshold ...
    };

    /// @brief 步骤回调（每完成一个步骤时调用）
    using StepCallback = std::function<void(const ReActStep&)>;

    /// @brief 流式 token 回调（Thought 阶段每个 delta 调用）
    using TokenCallback = std::function<void(const std::string& content_delta,
                                             const std::string& reasoning_delta)>;

    /// @brief 构造
    /// @param provider 推理提供者（非拥有，ChatSession 拥有）
    /// @param registry 工具注册表
    /// @param config 循环配置
    ReActLoop(ICompletionProvider* provider,
              std::shared_ptr<tool::ToolRegistry> registry,
              Config config = {});

    /// @brief 执行 ReAct 循环
    /// @param messages 对话历史（会被修改：追加 assistant + tool_result 消息）
    /// @param system_prompt 系统提示词
    /// @param tools_schema 工具 schema 数组（注入到 CompletionRequest）
    /// @param should_cancel 外部取消信号
    /// @param on_step 步骤回调（可选）
    /// @param on_token 流式 token 回调（可选）
    /// @return 循环执行结果
    ReActResult run(
        std::vector<ChatMessage>& messages,
        const std::string& system_prompt,
        const nlohmann::json& tools_schema,
        const std::atomic<bool>& should_cancel,
        StepCallback on_step = nullptr,
        TokenCallback on_token = nullptr
    );

private:
    /// @brief 执行 Thought 阶段（流式读取 LLM 响应）
    /// @param request 推理请求
    /// @param should_cancel 取消信号
    /// @param on_token token 回调
    /// @return Thought 结果（content, reasoning, tool_uses, 状态）
    struct ThoughtResult {
        std::string content;
        std::string reasoning;
        std::vector<ToolUse> tool_uses;
        int32_t prompt_tokens = 0;
        int32_t generated_tokens = 0;
        double prompt_ms = 0.0;
        double generation_ms = 0.0;
        enum { Completed, Error, Cancelled } status = Completed;
    };
    ThoughtResult execute_thought(
        const CompletionRequest& request,
        const std::atomic<bool>& should_cancel,
        TokenCallback on_token
    );

    /// @brief 构建 CompletionRequest
    CompletionRequest build_request(
        const std::vector<ChatMessage>& messages,
        const std::string& system_prompt,
        const nlohmann::json& tools_schema
    ) const;

    ICompletionProvider* m_provider;               // 非拥有
    std::shared_ptr<tool::ToolRegistry> m_registry;
    std::unique_ptr<tool::ToolExecutor> m_executor;
    Config m_config;
};

} // namespace agent
```

### run() 主循环伪代码

```
run(messages, system_prompt, tools_schema, should_cancel, on_step, on_token):
    start_time = now()
    total_tool_calls = 0

    for iteration = 1 to max_iterations:
        // === Thought 阶段 ===
        request = build_request(messages, system_prompt, tools_schema)
        thought = execute_thought(request, should_cancel, on_token)

        if thought.status == Cancelled:
            return ReActResult{ was_interrupted=true, ... }

        if thought.status == Error:
            return ReActResult{ was_error=true, error_message=..., ... }

        // 记录 Thought 步骤
        step = ReActStep{ type=Thought, step_number=iteration*3-2, thought_text=thought.content, ... }
        on_step(step)

        // === 终止判断：无 tool_use → FinalAnswer ===
        if thought.tool_uses.empty():
            messages.push_back(assistant(thought.content))
            return ReActResult{
                final_answer = thought.content,
                final_reasoning = thought.reasoning,
                total_iterations = iteration,
                ...
            }

        // === 有 tool_use：构建 assistant 消息 ===
        assistant_msg = assistant(thought.content)
        assistant_msg.reasoning = thought.reasoning
        assistant_msg.tool_uses = thought.tool_uses
        messages.push_back(assistant_msg)

        // 发布本轮 StreamDoneEvent（让 UI 知道本轮 LLM 输出结束）

        // === Action + Observation 阶段 ===
        for each tool_use in thought.tool_uses:
            // Action
            step = ReActStep{ type=Action, tool_name=tool_use.name, tool_input=tool_use.input, ... }
            on_step(step)
            exec_result = m_executor->execute(tool_use.name, tool_use.input, ctx)

            // Observation
            result_text = exec_result.result.to_string()
            messages.push_back(tool_result(tool_use.id, tool_use.name, result_text))
            step = ReActStep{ type=Observation, observation=result_text, is_error=exec_result.is_error, ... }
            on_step(step)

            total_tool_calls++

        // 继续下一轮 Thought

    // 超过最大迭代数
    return ReActResult{ was_error=true, error_message="max iterations reached", ... }
```

---

## 四、ChatSession 重构设计

### 重构前后对比

**重构前** — ChatSession.run_completion() 内联 while 循环：
```
run_completion()
  └─ TaskManager.launch()
       └─ while (iteration < 25) {
            build_request() / submit / stream / tool_execute / ...
          }
```

**重构后** — ChatSession 委托给 ReActLoop：
```
run_completion()
  └─ TaskManager.launch()
       └─ ReActLoop loop(provider, registry, config)
          result = loop.run(messages, system_prompt, tools, should_cancel, on_step, on_token)
          └─ handle result (retry / cancel / done)
```

### ChatSession 修改

```cpp
// chat_session.h 新增
private:
    // 原 build_request() 移至 ReActLoop（ChatSession 不再需要）
    // 新增: 创建 ReActLoop 配置
    ReActLoop::Config build_react_config() const;
```

```cpp
// chat_session.cpp run_completion 重构
void ChatSession::run_completion(const std::string& user_text, int retry_attempt) {
    if (retry_attempt == 0) {
        m_messages.push_back(ChatMessage::user(user_text));
    }

    EventBus::instance().publish_async(BackendStatusEvent{...});
    m_generating.store(true);

    auto task = TaskManager::instance().launch("completion",
        [this, user_text, retry_attempt]
        (const std::atomic<bool>& should_cancel) {
            // 创建 ReActLoop
            ReActLoop loop(m_provider.get(), m_tool_registry, build_react_config());

            // 准备 tools schema
            nlohmann::json tools = m_tool_registry
                ? m_tool_registry->get_all_schemas()
                : nlohmann::json::array();

            // 步骤回调：发布 Agent 事件
            auto on_step = [](const ReActStep& step) {
                switch (step.type) {
                    case ReActStepType::Thought:
                        EventBus::instance().publish_async(AgentStepEvent{...});
                        break;
                    case ReActStepType::Action:
                        EventBus::instance().publish_async(ToolCallEvent{...});
                        break;
                    case ReActStepType::Observation:
                        EventBus::instance().publish_async(ToolResultEvent{...});
                        break;
                    default: break;
                }
            };

            // token 回调：发布 StreamTokenEvent
            auto on_token = [](const std::string& content_delta,
                               const std::string& reasoning_delta) {
                EventBus::instance().publish_async(StreamTokenEvent{
                    .session_id = "default",
                    .content_delta = content_delta,
                    .reasoning_delta = reasoning_delta,
                    .is_thinking = !reasoning_delta.empty(),
                    .token_count = 0
                });
            };

            // 执行 ReAct 循环
            auto result = loop.run(
                m_messages, m_system_prompt, tools,
                should_cancel, on_step, on_token
            );

            // 处理结果
            if (result.was_interrupted) {
                EventBus::instance().publish_async(StreamDoneEvent{
                    .was_interrupted = true, ...
                });
            } else if (result.was_error) {
                // retry 逻辑（与当前相同）
                if (retry_attempt < m_max_retries) {
                    // ... retry ...
                } else {
                    EventBus::instance().publish_async(StreamErrorEvent{...});
                }
            } else {
                // 成功完成
                EventBus::instance().publish_async(StreamDoneEvent{
                    .full_content = result.final_answer,
                    .full_reasoning = result.final_reasoning,
                    .was_interrupted = false,
                    .prompt_tokens = result.prompt_tokens,
                    .generated_tokens = result.generated_tokens,
                    ...
                });
                EventBus::instance().publish_async(AgentDoneEvent{
                    .final_response = result.final_answer,
                    .total_steps = static_cast<int32_t>(result.steps.size()),
                    .total_tool_calls = result.total_tool_calls,
                    .total_duration_ms = result.total_duration_ms
                });
            }

            m_generating.store(false);
        },
        TaskType::Normal
    );
}
```

---

## 五、事件系统打通

### 事件发布时序

```
ReActLoop iteration 1:
  ┌─ Thought ──────────────────────────────────────────────────┐
  │  StreamTokenEvent(content_delta="Let me read...")           │
  │  StreamTokenEvent(content_delta="the file first.")          │
  │  AgentStepEvent(step_number=1, description="Thought: ...")  │
  └─────────────────────────────────────────────────────────────┘
  ┌─ Action ───────────────────────────────────────────────────┐
  │  ToolCallEvent(tool_name="Read", arguments={...})           │
  │  StreamTokenEvent(content_delta="\n[Tool: Read]\n")        │
  └─────────────────────────────────────────────────────────────┘
  ┌─ Observation ──────────────────────────────────────────────┐
  │  StreamTokenEvent(content_delta="[Result]: ...")           │
  │  ToolResultEvent(call_id="...", result="...", is_error=false)│
  └─────────────────────────────────────────────────────────────┘

ReActLoop iteration 2:
  ┌─ Thought ──────────────────────────────────────────────────┐
  │  StreamTokenEvent(content_delta="Now I'll write...")        │
  │  AgentStepEvent(step_number=4, description="Thought: ...")  │
  └─────────────────────────────────────────────────────────────┘
  ┌─ Action ───────────────────────────────────────────────────┐
  │  ToolCallEvent(tool_name="Write", arguments={...})          │
  └─────────────────────────────────────────────────────────────┘
  ┌─ Observation ──────────────────────────────────────────────┐
  │  ToolResultEvent(call_id="...", result="...", is_error=false)│
  └─────────────────────────────────────────────────────────────┘

ReActLoop iteration 3:
  ┌─ Thought (FinalAnswer) ────────────────────────────────────┐
  │  StreamTokenEvent(content_delta="Done! The file...")        │
  │  AgentStepEvent(step_number=7, description="FinalAnswer")   │
  └─────────────────────────────────────────────────────────────┘

  StreamDoneEvent(full_content="Done! The file...", ...)
  AgentDoneEvent(final_response="Done! The file...", total_steps=7, total_tool_calls=2)
```

### ToolType 映射

`ToolCallEvent` 需要 `ToolType` 枚举，需从工具名映射：

```cpp
ToolType infer_tool_type(const std::string& tool_name) {
    if (tool_name == "Read")   return ToolType::ReadFile;
    if (tool_name == "Write")  return ToolType::WriteFile;
    if (tool_name == "Edit")   return ToolType::EditFile;
    if (tool_name == "Bash")   return ToolType::Execute;
    if (tool_name == "Grep" || tool_name == "Glob") return ToolType::Search;
    if (tool_name == "Agent")  return ToolType::Agent;
    return ToolType::Other;
}
```

---

## 六、文件变更清单

### 新增文件

| 文件 | 内容 |
|------|------|
| `src/agent/core/react_loop.h` | ReActLoop 完整类定义（替换空 stub） |
| `src/agent/core/react_loop.cpp` | ReActLoop 实现（Thought/Action/Observation） |

### 修改文件

| 文件 | 变更 |
|------|------|
| `src/agent/core/chat_session.h` | 移除 `build_request()`，新增 `build_react_config()` |
| `src/agent/core/chat_session.cpp` | `run_completion()` 重构为委托 ReActLoop |
| `CMakeLists.txt` | 添加 `src/agent/core/react_loop.cpp` |

### 不变文件

| 文件 | 原因 |
|------|------|
| `chat_types.h` | ToolUse/ChatMessage/StreamChunk 已完备 |
| `anthropic_adapter.cpp` | 协议适配层不变 |
| `openai_adapter.cpp` | 协议适配层不变 |
| `main.cpp` | 工具注册不变（已在上一步完成） |
| `message/types.h` | 事件类型已定义，无需修改 |

---

## 七、实施阶段

### Phase 1: ReActLoop 核心实现

**目标**: 实现 react_loop.h / react_loop.cpp，不修改 ChatSession

**步骤**:

1. 编写 `react_loop.h`：
   - `ReActStepType` 枚举
   - `ReActStep` 结构体
   - `ReActResult` 结构体
   - `ReActLoop` 类（Config, StepCallback, TokenCallback, run()）

2. 编写 `react_loop.cpp`：
   - `build_request()` — 从 ChatSession 移入
   - `execute_thought()` — 从 ChatSession 流式读取逻辑提取
   - `run()` — 主循环，Thought/Action/Observation 三阶段
   - 工具执行逻辑 — 从 ChatSession 移入

3. 更新 `CMakeLists.txt`：添加 `src/agent/core/react_loop.cpp`

4. 编译验证 ReActLoop 独立可编译

### Phase 2: ChatSession 重构

**目标**: ChatSession 委托 ReActLoop，移除内联 while 循环

**步骤**:

1. 修改 `chat_session.h`：
   - 移除 `build_request()` 声明
   - 新增 `build_react_config()` 声明
   - 新增 `#include "agent/core/react_loop.h"`

2. 重写 `chat_session.cpp` 的 `run_completion()`：
   - 创建 ReActLoop 实例
   - 定义 on_step 回调（发布 Agent 事件）
   - 定义 on_token 回调（发布 StreamTokenEvent）
   - 调用 `loop.run()`
   - 处理 ReActResult（retry / cancel / done）

3. 移除 `chat_session.cpp` 中的旧代码：
   - `build_request()` 方法
   - `PendingToolUse` struct
   - 流式读取 while 循环
   - 工具执行 for 循环

4. 编译验证

### Phase 3: 事件系统验证

**目标**: 确认所有事件正确发布

**验证清单**:
- [ ] Thought 阶段发布 StreamTokenEvent（content/reasoning delta）
- [ ] Thought 阶段结束发布 AgentStepEvent
- [ ] Action 阶段发布 ToolCallEvent + StreamTokenEvent（[Tool: name]）
- [ ] Observation 阶段发布 ToolResultEvent + StreamTokenEvent（[Result: ...]）
- [ ] 循环完成发布 StreamDoneEvent + AgentDoneEvent
- [ ] 错误时发布 StreamErrorEvent
- [ ] 取消时发布 StreamDoneEvent(was_interrupted=true)
- [ ] 超过 max_iterations 发布 StreamErrorEvent

---

## 八、终止条件处理

| 条件 | 行为 | ReActResult 字段 |
|------|------|-----------------|
| LLM 无 tool_use | 正常完成，返回 final_answer | was_error=false |
| 达到 max_iterations | 返回错误 | was_error=true, error_message="max iterations" |
| 用户取消 (should_cancel) | 立即返回 | was_interrupted=true |
| 流式错误 (StreamState::Error) | 返回错误，ChatSession 决定是否重试 | was_error=true |
| 提交失败 (submit 返回 nullptr) | 返回错误，ChatSession 决定是否重试 | was_error=true |
| 工具执行错误 | 不终止循环，将错误作为 Observation 回传 LLM | — (循环继续) |

**关键设计**: 工具执行错误不终止循环 — LLM 收到错误结果后可以自行决定重试、换方案或向用户报告。这与 Claude Code 行为一致。

---

## 九、与当前实现的差异对比

| 方面 | 当前 while 循环 | ReActLoop |
|------|----------------|-----------|
| 结构 | 扁平 while 循环 | 显式 Thought/Action/Observation 三阶段 |
| 步骤追踪 | 无 | ReActStep 记录每一步 |
| 事件 | 仅 StreamTokenEvent + StreamDoneEvent | + AgentStepEvent + ToolCallEvent + ToolResultEvent + AgentDoneEvent |
| 关注点分离 | ChatSession 混合会话管理 + 循环逻辑 | ChatSession 管理会话，ReActLoop 管循环 |
| 可测试性 | 需要模拟整个 ChatSession | ReActLoop 可独立测试 |
| 可扩展性 | 难以添加新阶段 | 可插入 Plan/Reflect 等新阶段 |
| 工具错误处理 | 作为 Observation 回传（已实现） | 同（保持一致） |
| 流式输出 | 直接 publish StreamTokenEvent | 通过 TokenCallback 回调 |
| retry | 在循环内部 | 在循环外部（ChatSession 处理） |

---

## 十、未来扩展（不在本计划范围）

| 扩展 | 说明 | 依赖 |
|------|------|------|
| `QueryEngine` | 会话生命周期编排器 | ReActLoop 完成后 |
| `SystemPromptBuilder` | 动态系统提示构建（含 ReAct 指令） | `system_prompt.h` stub |
| `ContextTruncator` | 上下文窗口管理 | `compact/truncator.h` stub |
| `TokenCounter` | 精确 token 估算 | `compact/token_count.h` stub |
| `PermissionChecker` | 交互式权限确认 | `permission/checker.h` stub |
| Plan 阶段 | 在 Thought 前增加 Plan 步骤 | ReActLoop 三阶段基础上扩展 |
| Reflect 阶段 | 在 Observation 后增加 Reflect 步骤 | ReActLoop 三阶段基础上扩展 |
