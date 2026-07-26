# WorkX 架构重构方案 — 事件驱动 + 任务系统 + ReAct Agent

> **状态**: 待实施  
> **范围**: 聚焦三大子系统的架构级重构（非细节修 bug）  
> **前提**: ReActLoop（react_loop.h/cpp）已实现，本方案在其基础上做系统性改进  
> **目标**: 解耦单例依赖、修复任务系统缺陷、提升 Agent 并行能力与可观测性

---

## 0. 诊断总览

### 0.1 原诊断（已全部经代码验证为真实存在）

| 子系统 | 核心问题 | 严重度 | 现有计划覆盖？ |
|--------|---------|--------|-------------|
| **EventBus** | 全局单例、无自动消费机制、事件类型散落 | 🔴 P0 | ❌ 未覆盖 |
| **TaskManager** | `update()` 无人调用、无线程池、取消信号不传递到 Tool | 🔴 P0 | ❌ 未覆盖 |
| **ReActLoop** | 工具串行执行、无上下文压缩、Observation 无截断 | 🟠 P1 | ❌ 未覆盖 |
| **ChatSession** | 回调 lambda 过重、`session_id` 硬编码、重试递归 | 🟠 P1 | ⚠️ 部分覆盖（PHASE4） |
| **CMake/目录** | 源文件手动枚举、无子模块 CMake | 🟡 P2 | ❌ 未覆盖 |

### 0.2 架构探索新发现（文档原未覆盖，本次补充）

| 编号 | 子系统 | 核心问题 | 严重度 | 处理方式 |
|------|--------|---------|--------|---------|
| **X-1** | 测试覆盖 | TaskManager/EventBus/ReActLoop/ToolExecutor 零单元测试 | 🔴 P0 | 本方案 Phase 0.5 |
| **X-2** | 测试覆盖 | 集成测试依赖手动启动 Python server，CI 不可重复 | 🟠 P1 | 本方案 Phase 6 |
| **T-2** | 线程安全 | 工具类无可重入性保证（PLAN 3.1 并行执行的前置条件） | 🟠 P1 | 本方案 Phase 3（新增） |
| **K-1** | 线程安全 | ToolExecutor::execute() const 但 call() 可能修改状态 | 🟠 P1 | 本方案 Phase 3（新增） |
| **K-2** | 事件系统 | Terminal 非唯一入口，Client::chat() 脚本场景不走主循环 | 🟠 P1 | 本方案 Phase 1 扩展 |
| **C-1** | 配置管理 | ConfigManager 也是单例，与 EventBus 同病 | 🟠 P1 | 本方案 Phase 4 扩展 |
| **D-1** | 依赖注入 | TaskManager 也是单例，测试无法注入线程池 mock | 🟠 P1 | 本方案 Phase 2 扩展 |
| **L-4** | 线程安全 | Task::m_progress 也非原子（PLAN 2.4 只改 m_status 不完整） | 🟠 P1 | 本方案 Phase 1 扩展 |
| **G-1** | 日志系统 | 核心模块几乎无日志（全项目仅 3 处 LOG） | 🟠 P1 | 每个 Phase 顺手补 |
| **E-1** | 错误处理 | 4 种错误风格并存（Result/struct bool/异常混用） | 🟠 P1 | **独立立项 v2** |
| **E-2** | 错误处理 | Result::unwrap() 抛异常是反模式 | 🟠 P1 | **独立立项 v2** |
| **E-3** | 错误处理 | ChatSession::send_message() 返回 void，同步调用方无法感知失败 | 🟠 P1 | **独立立项 v2** |
| **L-1** | 生命周期 | g_backend 裸指针跨函数传递所有权 | 🟠 P1 | 长期技术债（Phase 6+） |
| **L-2** | 生命周期 | ReActLoop::m_provider 裸指针依赖文档约束 | 🟠 P1 | 长期技术债（Phase 6+） |
| **T-1** | 线程安全 | EventBus publish() 持锁调回调，重入死锁风险 | 🟠 P1 | 本方案 Phase 4 |
| **T-3** | 线程安全 | ChatRenderer 多数字段非原子 | 🟠 P1 | 本方案 Phase 3.5 |

> P2/P3 共 14 条（L-3/L-5/T-4/T-5/T-6/C-2/C-3/G-2/G-3/G-4/H-1/H-2/H-3/H-4），详见探索报告，不在此表展开。

**与现有计划的关系**：
- `react-loop-plan.md` ✅ 已完成（ReActLoop 提取）
- `PLAN_PHASE4.md` 🔧 待实施（代码质量 P2/P3，本方案不重复）
- 本方案 🆕 聚焦 **架构级** 问题，与 PHASE4 正交、可并行
- **`ARCH_REFACTOR_PLAN_V2.md`** 📋 独立项（错误处理统一，本方案完成后启动）

---

## 一、事件驱动子系统重构

### 1.1 引入 `IEventBus` 接口，逐步替代单例

**问题**：`EventBus::instance()` 导致模块间隐式耦合，单元测试无法 mock。

**方案**：定义接口 + 默认单例实现，允许注入。

```cpp
// core/events/i_event_bus.h  [新增]
#pragma once
#include <functional>
#include <typeindex>
#include "event_token.h"

namespace agent {

template<typename T>
using EventCallback = std::function<void(const T&)>;

class IEventBus {
public:
    virtual ~IEventBus() = default;

    template<typename T>
    virtual EventToken subscribe(EventCallback<T> callback) = 0;

    template<typename T>
    virtual void unsubscribe(const EventToken& token) = 0;

    template<typename T>
    virtual void publish(const T& event) = 0;

    template<typename T>
    virtual void publish_async(const T& event) = 0;

    virtual void process_async_events() = 0;
    virtual void clear() = 0;
};

} // namespace agent
```

```cpp
// core/events/event_bus.h  [修改]
// EventBus 继承 IEventBus，保持 instance() 作为默认全局实例
class EventBus final : public IEventBus {
public:
    static EventBus& instance() noexcept;
    // ... 其余不变
};
```

**迁移策略（渐进式，不一次性全改）**：

| 阶段 | 动作 | 涉及文件 |
|------|------|---------|
| 1 | 新增 `IEventBus` 接口，`EventBus` 继承它 | `core/events/` |
| 2 | `ChatSession` 构造函数增加 `IEventBus&` 参数，默认用 `EventBus::instance()` | `chat_session.h/cpp` |
| 3 | `TaskManager` 增加 `IEventBus&` 成员，任务事件发布走注入的 bus | `task_manager.h/cpp` |
| 4 | `main.cpp` 组装时显式传入 `EventBus::instance()` | `main.cpp` |
| 5 | 测试代码创建 `MockEventBus` 注入 | `tests/` |

### 1.2 修复 `publish_async` 自动消费

**问题**：异步事件入队后无人调用 `process_async_events()`，事件可能永久滞留。

**方案A（推荐）**：在 `Terminal::run()` 主循环中每帧消费：

```cpp
// tui/core/terminal.cpp — run() 主循环内
while (m_running) {
    // 原有：处理输入、渲染...
    
    // 新增：消费异步事件
    EventBus::instance().process_async_events();
    
    // 新增：清理已完成任务（见第二章）
    TaskManager::instance().update();
    
    std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60fps
}
```

**方案B（兜底）**：`publish_async` 改为同步发布（牺牲一点性能换取正确性）：

```cpp
// event_bus.h
void publish_async(const T& event) {
    // 直接同步发布，不经过队列
    publish(event);
}
```

> **决策**：采用方案A，保持异步语义，在终端主循环中统一消费。若主循环不是每帧调用，则回退到方案B。

### 1.3 事件类型归并到统一命名空间

**问题**：事件定义散落在 `agent/message/types.h` 和 `agent/api/chat_types.h`。

**方案**：新建 `core/events/events.h`，集中所有事件类型，保留旧文件做 `[[deprecated]]` 转发。

```cpp
// core/events/events.h  [新增]
#pragma once
#include <string>
#include <memory>

namespace agent {

// === 用户事件 ===
struct UserInputEvent { std::string text; };
struct InterruptEvent { bool force = false; };

// === 流式事件 ===
struct StreamTokenEvent {
    std::string session_id;
    std::string content_delta;
    std::string reasoning_delta;
    bool is_thinking = false;
    int32_t token_count = 0;
};
struct StreamDoneEvent { /* ... */ };
struct StreamErrorEvent { /* ... */ };

// === Agent 事件 ===
struct AgentStepEvent { std::string step_id; int32_t step_number; std::string description; };
struct ToolCallEvent { std::string tool_name; std::string arguments; std::string call_id; };
struct ToolResultEvent { std::string call_id; std::string result; bool is_error = false; };
struct AgentDoneEvent { std::string final_response; int32_t total_steps; int32_t total_tool_calls; double total_duration_ms; };

// === 任务事件 ===
struct TaskStartedEvent { std::shared_ptr<class Task> task; std::string task_name; };
struct TaskCompletedEvent { std::shared_ptr<class Task> task; std::string task_name; float duration_ms; };
struct TaskFailedEvent { std::shared_ptr<class Task> task; std::string task_name; std::string error_message; float duration_ms; };
struct TaskCancelledEvent { std::shared_ptr<class Task> task; std::string task_name; float duration_ms; };

// === 系统事件 ===
struct BackendStatusEvent { enum Status { Disconnected, Connecting, Connected, Error } status; std::string backend_name; std::string error; };
struct ShutdownEvent { bool force = false; };

} // namespace agent
```

---

## 二、任务系统重构

### 2.1 修复 `TaskManager::update()` 调用缺失

**问题**：`update()` 清理已完成任务的入口从未被调用，导致 `m_entries` 无限增长。

**方案**：在终端主循环中每帧调用（与 1.2 一起）。

```cpp
// main.cpp / Terminal::run() 中
while (running) {
    EventBus::instance().process_async_events();
    TaskManager::instance().update();  // 清理已完成任务，join 线程
    
    // 原有输入处理...
}
```

### 2.2 引入线程池替代裸 `std::thread`

**问题**：每个 `Task` 直接创建 `std::thread`，高并发时线程数无上限。

**方案**：`TaskManager` 内部持有一个固定大小线程池。

```cpp
// core/task/thread_pool.h  [新增]
#pragma once
#include <thread>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>

namespace agent {

class ThreadPool {
public:
    explicit ThreadPool(size_t num_threads = std::thread::hardware_concurrency());
    ~ThreadPool();

    void enqueue(std::function<void()> task);
    void shutdown();
    size_t active_count() const;

private:
    std::vector<std::thread> m_workers;
    std::queue<std::function<void()>> m_tasks;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::atomic<bool> m_stop{false};
    std::atomic<size_t> m_active{0};
};

} // namespace agent
```

```cpp
// task_manager.h  [修改]
class TaskManager final {
    // ...
private:
    std::unique_ptr<ThreadPool> m_pool;  // 替代裸 thread
    std::vector<std::shared_ptr<Task>> m_tasks;  // 仅追踪，不持 thread
};
```

```cpp
// task_manager.cpp  [修改]
void TaskManager::start(std::shared_ptr<Task> task) {
    if (!task) return;
    
    if (task->getType() == TaskType::Blocking) {
        task->execute();
        return;
    }
    
    {
        std::lock_guard<std::mutex> lock(m_tasks_mutex);
        m_tasks.push_back(task);
    }
    
    m_pool->enqueue([this, task]() {
        task->execute();
        m_tasks_cv.notify_all();
    });
}
```

**线程池大小**：默认 `hardware_concurrency()`，可通过配置覆盖。

### 2.3 修复取消信号传递到 ToolContext

**问题**：`Task::m_should_cancel` 和 `ToolContext::cancelled_` 是两个独立的标志，Tool 执行时检测不到 Task 的取消。

**方案**：让 `ToolContext` 持有 `Task` 取消标志的引用。

```cpp
// agent/tool/context.h  [修改]
struct ToolContext {
    std::string cwd;
    std::string session_id;
    std::string request_id;
    std::string model;
    nlohmann::json options;
    
    // 取消信号改为外部注入的原子引用
    std::shared_ptr<std::atomic<bool>> cancel_token;
    
    bool is_cancelled() const {
        return cancel_token && cancel_token->load(std::memory_order_relaxed);
    }
};
```

```cpp
// task_manager.cpp  [修改]
void Task::execute() {
    // ...
    ToolContext ctx;
    ctx.cwd = std::filesystem::current_path().string();
    ctx.session_id = "default";
    ctx.cancel_token = std::make_shared<std::atomic<bool>>(false);
    // 绑定到 Task 的取消标志
    // 方式：在 cancel() 时同时设置 ctx.cancel_token
}
```

**更简单的方案**：`ToolContext` 直接暴露 `std::atomic<bool>*` 指针：

```cpp
struct ToolContext {
    // ...
    const std::atomic<bool>* cancel_flag = nullptr;
    
    bool is_cancelled() const {
        return cancel_flag && cancel_flag->load(std::memory_order_relaxed);
    }
};
```

```cpp
// ReActLoop 中创建 ToolContext 时传入
void ReActLoop::run(...) {
    // ...
    for (const auto& tu : thought.tool_uses) {
        ToolContext ctx;
        ctx.cancel_flag = &should_cancel;  // 直接引用外部取消信号
        // ...
    }
}
```

### 2.4 修复 `Task::setProgress()` 状态竞争

**问题**：`setProgress()` 中 `m_status = Completed` 可能与 `execute()` 并发修改状态。

**方案**：`m_status` 改为 `std::atomic<TaskStatus>`，所有状态变更用 `compare_exchange` 或顺序一致的原子操作。

```cpp
// task_manager.h  [修改]
class Task : public std::enable_shared_from_this<Task> {
    // ...
    std::atomic<TaskStatus> m_status{TaskStatus::Pending};
    
    void setStatus(TaskStatus s) {
        m_status.store(s, std::memory_order_release);
    }
    TaskStatus getStatus() const {
        return m_status.load(std::memory_order_acquire);
    }
};
```

---

## 三、ReAct Agent 增强

### 3.1 并行工具执行（最大性能提升点）

**问题**：LLM 支持并行 function calling，但当前 `for` 循环串行执行。

**方案**：在 ReActLoop Action 阶段使用 `std::async` 并行执行独立的 tool_use。

```cpp
// react_loop.cpp  [修改] — Action + Observation 阶段

// === Action + Observation 阶段（并行版）===
if (!m_executor) { /* 错误处理，同前 */ }

// 构建共享上下文
ToolContext ctx;
ctx.cwd = std::filesystem::current_path().string();
ctx.session_id = "default";
ctx.cancel_flag = &should_cancel;

// 1. 并行执行所有 tool_use
struct ToolExecution {
    std::string tool_use_id;
    std::string tool_name;
    nlohmann::json tool_input;
    std::future<std::pair<std::string, bool>> future;  // {result_text, is_error}
};

std::vector<ToolExecution> executions;
executions.reserve(thought.tool_uses.size());

for (const auto& tu : thought.tool_uses) {
    // 记录 Action 步骤（同步，UI 即时反馈）
    {
        ReActStep step;
        step.type = ReActStepType::Action;
        step.step_number = ++step_counter;
        step.tool_name = tu.name;
        step.tool_input = tu.input;
        result.steps.push_back(step);
        if (on_step) on_step(step);
    }
    
    // 异步执行
    auto future = std::async(std::launch::async, [this, &ctx, &tu]() {
        try {
            auto exec_result = m_executor->execute(tu.name, tu.input, ctx);
            return std::make_pair(exec_result.result.to_string(), exec_result.is_error);
        } catch (const std::exception& e) {
            return std::make_pair(
                std::format("Error: tool '{}' threw exception: {}", tu.name, e.what()), true);
        }
    });
    
    executions.push_back({tu.id, tu.name, tu.input, std::move(future)});
    result.total_tool_calls++;
}

// 2. 等待所有工具完成，按原始顺序生成 Observation
for (auto& exec : executions) {
    auto [result_text, tool_error] = exec.future.get();
    
    // 添加 tool_result 消息
    messages.push_back(ChatMessage::tool_result(exec.tool_use_id, exec.tool_name, result_text));
    
    // 记录 Observation 步骤
    {
        ReActStep step;
        step.type = ReActStepType::Observation;
        step.step_number = ++step_counter;
        step.observation = result_text;
        step.is_error = tool_error;
        result.steps.push_back(step);
        if (on_step) on_step(step);
    }
}
```

**限制**：
- 若工具间有依赖（如先 `Read` 后 `Edit` 同一文件），LLM 应分两轮调用，而非同一轮并行。并行执行只针对同一轮内 LLM 明确同时请求的 tools。
- `BashTool` 若命令间有依赖，也可能出问题。但 LLM 通常不会在同一个 `tool_use` 块中发送有副作用依赖的命令。

### 3.2 引入 `IReActObserver` 解耦 ChatSession 回调

**问题**：`ChatSession::run_completion()` 中 `on_step` lambda 长达 50+ 行，ReActLoop 无法脱离 EventBus 体系独立使用。

**方案**：定义观察者接口，ChatSession 实现做事件转换。

```cpp
// agent/core/react_observer.h  [新增]
#pragma once
#include "react_loop.h"

namespace agent {

class IReActObserver {
public:
    virtual ~IReActObserver() = default;
    
    virtual void on_thought(const ReActStep& step) = 0;
    virtual void on_action(const ReActStep& step) = 0;
    virtual void on_observation(const ReActStep& step) = 0;
    virtual void on_final_answer(const ReActStep& step) = 0;
    virtual void on_token(const std::string& content_delta, const std::string& reasoning_delta) = 0;
};

} // namespace agent
```

```cpp
// agent/core/chat_session.cpp  [修改] — 内部类实现观察者
class ChatSession::ReActEventPublisher : public IReActObserver {
public:
    explicit ReActEventPublisher(std::string session_id) 
        : m_session_id(std::move(session_id)) {}
    
    void on_thought(const ReActStep& step) override {
        EventBus::instance().publish_async(AgentStepEvent{
            .step_id = std::format("thought-{}", step.step_number),
            .step_number = step.step_number,
            .description = "(thinking)"
        });
        if (!step.tool_uses.empty()) {
            EventBus::instance().publish_async(StreamDoneEvent{
                .session_id = m_session_id,
                .full_content = step.thought_text,
                .full_reasoning = step.reasoning,
                .was_interrupted = false,
                .generation_ms = step.duration_ms
            });
        }
    }
    
    void on_action(const ReActStep& step) override {
        EventBus::instance().publish_async(ToolCallEvent{
            .tool_name = step.tool_name,
            .arguments = step.tool_input.dump(),
            .tool_type = infer_tool_type(step.tool_name)
        });
    }
    
    void on_observation(const ReActStep& step) override {
        EventBus::instance().publish_async(ToolResultEvent{
            .call_id = "",
            .result = step.observation,
            .is_error = step.is_error
        });
    }
    
    void on_final_answer(const ReActStep& step) override {
        // 由 ChatSession 统一处理
    }
    
    void on_token(const std::string& cd, const std::string& rd) override {
        EventBus::instance().publish_async(StreamTokenEvent{
            .session_id = m_session_id,
            .content_delta = cd,
            .reasoning_delta = rd,
            .is_thinking = !rd.empty()
        });
    }

private:
    std::string m_session_id;
};
```

```cpp
// ReActLoop::run() 接口扩展 [修改]
ReActResult run(
    std::vector<ChatMessage>& messages,
    const std::string& system_prompt,
    const nlohmann::json& tools_schema,
    const std::atomic<bool>& should_cancel,
    IReActObserver* observer = nullptr  // 替代 StepCallback + TokenCallback
);
```

### 3.3 上下文压缩集成

**问题**：`build_request()` 直接拼接全量 `messages`，无截断/压缩。

**方案**：在 `build_request()` 前加入 `ContextCompressor`。

```cpp
// agent/compact/context_compressor.h  [新增]
#pragma once
#include <vector>
#include "agent/api/chat_types.h"

namespace agent {

class ContextCompressor {
public:
    struct Config {
        int max_messages = 50;           // 最大保留消息数
        int max_tokens_estimate = 8000;  // 估计 token 上限（粗略）
        bool compress_old_tools = true;  // 压缩旧的 tool_result
    };
    
    explicit ContextCompressor(Config cfg = {});
    
    // 压缩消息列表，返回压缩后的列表（不修改原始列表）
    std::vector<ChatMessage> compress(const std::vector<ChatMessage>& messages) const;

private:
    Config m_config;
};

} // namespace agent
```

**压缩策略**：
1. **保留**：最近 N 轮 Thought/Action/Observation（完整保留）
2. **压缩**：更早的 tool_result 替换为摘要（`[Result of Bash: 成功执行 5 条命令]`）
3. **丢弃**：超过 `max_messages` 的旧消息，仅保留 system + 最近 user message
4. **精确**：调用已有的 `token_count.h` 做粗略估算

```cpp
// react_loop.cpp  [修改]
CompletionRequest ReActLoop::build_request(
    const std::vector<ChatMessage>& messages,
    const std::string& system_prompt,
    const nlohmann::json& tools_schema
) const {
    CompletionRequest request;
    request.stream = true;
    
    if (!system_prompt.empty()) {
        request.messages.push_back(ChatMessage::system(system_prompt));
    }
    
    // 新增：上下文压缩
    auto compressed = m_compressor.compress(messages);
    for (const auto& msg : compressed) {
        request.messages.push_back(msg);
    }
    
    if (!tools_schema.is_null() && tools_schema.is_array() && !tools_schema.empty()) {
        request.tools = tools_schema;
    }
    
    return request;
}
```

### 3.4 Observation 结果截断

**问题**：工具结果（grep 大仓库、bash 长日志）可能直接撑爆上下文。

**方案**：在 `ToolExecutor` 层加入结果截断。

```cpp
// agent/tool/executor.h  [修改]
struct ExecutionResult {
    std::string tool_name;
    ToolResult result;
    bool is_error{false};
    bool was_truncated{false};  // 新增：结果是否被截断
};
```

```cpp
// agent/tool/executor.h  [修改 execute() 末尾]
// 新增：结果截断（在返回前）
static constexpr size_t MAX_RESULT_LENGTH = 8000;  // 可配置
if (exec_result.result.text.length() > MAX_RESULT_LENGTH) {
    std::string truncated = exec_result.result.text.substr(0, MAX_RESULT_LENGTH / 2)
        + "\n\n... [output truncated, " 
        + std::to_string(exec_result.result.text.length() - MAX_RESULT_LENGTH) 
        + " characters omitted] ...\n\n"
        + exec_result.result.text.substr(exec_result.result.text.length() - MAX_RESULT_LENGTH / 2);
    exec_result.result.text = std::move(truncated);
    exec_result.was_truncated = true;
}
```

> **注意**：截断策略应智能——保留头部和尾部（通常包含错误信息和总结），省略中间。不同工具可自定义截断策略。

---

## 四、目录与 CMake 模块化

### 4.1 按模块拆分 CMakeLists.txt

**问题**：根 CMakeLists.txt 230+ 行源文件手动枚举，新增文件需手动编辑。

**方案**：每个 `src/` 子目录自建 `CMakeLists.txt`。

```
CMakeLists.txt                    # 根：项目配置 + 子目录 + 外部依赖
src/
├── CMakeLists.txt               # 汇总 src 子模块
├── core/
│   └── CMakeLists.txt           # core 模块源文件
├── agent/
│   └── CMakeLists.txt           # agent 模块源文件
├── tui/
│   └── CMakeLists.txt           # tui 模块源文件
└── app/
    └── CMakeLists.txt           # app 模块（仅 main.cpp）
```

```cmake
# src/core/CMakeLists.txt
set(CORE_SOURCES
    task/task_manager.cpp
    config/config_manager.cpp
    # events/ 是 header-only，无 .cpp
)
add_library(workx_core INTERFACE)
target_sources(workx_core INTERFACE ${CORE_SOURCES})
target_include_directories(workx_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/..)
```

```cmake
# src/CMakeLists.txt
add_subdirectory(core)
add_subdirectory(agent)
add_subdirectory(tui)
add_subdirectory(app)

add_library(libworkx STATIC)
target_sources(libworkx PRIVATE
    $<TARGET_PROPERTY:workx_core,INTERFACE_SOURCES>
    $<TARGET_PROPERTY:workx_agent,INTERFACE_SOURCES>
    $<TARGET_PROPERTY:workx_tui,INTERFACE_SOURCES>
)
# 链接外部依赖...
```

### 4.2 源文件使用 `file(GLOB_RECURSE ...)`（可选）

若不想手动维护列表，可用 `GLOB_RECURSE`（CMake 不推荐，但适合快速迭代）：

```cmake
# src/agent/CMakeLists.txt
file(GLOB_RECURSE AGENT_SOURCES 
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
)
add_library(workx_agent INTERFACE)
target_sources(workx_agent INTERFACE ${AGENT_SOURCES})
```

> **注意**：GLOB 在新增文件时需重新运行 cmake configure，不适合 CI。开发阶段可用，发布前切回显式列表。

---

## 五、实施路线图

> **执行顺序调整说明**：基于架构探索发现，新增 Phase 0.5（补测试）与 Phase 3（工具线程安全审计），
> 扩展 Phase 1/2/4 覆盖新发现问题。每个 Phase 均包含日志补充任务（G-1）。

### Phase 0：准备（1 天）
- [ ] 备份当前代码（git tag `before-arch-refactor`）
- [ ] 确认所有测试可编译运行
- [ ] 创建 feature 分支 `arch-refactor`
- [ ] **日志（G-1）**：在 `lib/liblogger/logger.h` 确认 LOG_INFO/LOG_DEBUG/LOG_WARN/LOG_ERROR 宏可用；制定各模块日志埋点规范（含 session_id / task_id 上下文）

### Phase 0.5：补核心模块测试（3 天）🔴 P0 — 新增
> **目的**：建立回归网，后续 Phase 的重构需要测试保护。无测试不能重构。
- [ ] `tests/unit/core/events/test_event_bus.cpp`：subscribe/unsubscribe/publish/publish_async/process_async_events/clear
- [ ] `tests/unit/core/task/test_task_manager.cpp`：start/cancel/update/getRunningTasks/TaskStatus 状态机
- [ ] `tests/unit/agent/core/test_react_loop.cpp`：Thought/Action/Observation 三阶段、should_cancel 中断、max_iterations
- [ ] `tests/unit/agent/tool/test_tool_executor.cpp`：execute 权限检查、异常捕获、is_error 传播
- [ ] `tests/unit/agent/tool/test_tool_registry.cpp`：register/lookup/权限路由
- [ ] `tests/unit/tui/render/test_streaming_buffer.cpp`：push/stop/flush 多线程竞争
- [ ] `tests/unit/tui/render/test_spinner.cpp`：start/stop/join
- [ ] `tests/unit/tui/widgets/test_status_bar.cpp`：set_token_count/set_context_limit/set_cache_read_tokens 线程安全
- [ ] 补 `MockProvider`：模拟流式 chunk、工具调用、错误响应（替换当前 test_chat_session.cpp:17-35 的简化版）
- [ ] **日志（G-1）**：测试失败时输出 EventBus 队列状态、TaskManager 任务列表，便于定位

### Phase 1：P0 紧急修复（2 天）
- [ ] **1.2** 在 `Terminal::run()` 中加入 `process_async_events()` 和 `TaskManager::update()` 调用
- [ ] **2.1** 同上（已合并到 1.2）
- [ ] **2.3** 修复 `ToolContext` 取消信号：传递 `should_cancel` 引用
- [ ] **2.4** `TaskStatus` 改为原子类型
- [ ] **L-4 扩展**：`Task::m_progress` / `m_max_progress` 也改为 `std::atomic<float>`（PLAN 2.4 只改 m_status 不完整）
- [ ] **K-2 扩展**：非 TUI 场景（`Client::chat()` / `chat_async()`）也需消费异步事件
  - 方案：`Client` 内部启动轻量后台泵线程，或在 `chat()` 同步阻塞期间定期 `process_async_events()` + `TaskManager::update()`
  - 单元测试场景：`MockEventBus` 同步发布，不依赖泵线程
- [ ] 编译验证 + 运行 Phase 0.5 的测试
- [ ] **日志（G-1）**：在 `EventBus::publish_async` 入队、`process_async_events` 消费、`TaskManager::start/update` 处加 LOG_DEBUG，统计队列积压

### Phase 2：任务系统增强（2 天）
- [ ] **2.2** 实现 `ThreadPool`，替换 `TaskManager` 裸线程
- [ ] `TaskManager` 适配线程池接口
- [ ] **D-1 扩展**：`TaskManager` 单例 DI 化
  - 抽取 `ITaskManager` 接口（或直接改为可注入的普通类）
  - `ChatSession` / `Client` 构造函数接收 `TaskManager&`（或 `ITaskManager&`）
  - `main.cpp` 显式组装，测试用 `MockTaskManager` 注入
- [ ] 压力测试：并发启动 20+ 个 Task，验证线程数受限
- [ ] **日志（G-1）**：ThreadPool 任务入队/出队/执行耗时/异常；Task 状态变更链路

### Phase 3：工具线程安全审计（2 天）🟠 P1 — 新增
> **目的**：PLAN 3.1 并行执行的先决条件。无审计直接并行化会引发数据竞争。
- [ ] **T-2 审计**：逐个检查工具类的 `call()` 是否可重入
  - `BashTool`：cwd/env 状态是否线程本地（当前未实现，需在实现时保证）
  - `FileReadTool`：无状态，✅ 安全
  - `FileWriteTool` / `FileEditTool`：`file_history` 有 mutex，但需审计并行写不同文件时的历史交叉
  - `GlobTool` / `GrepTool`：无状态，✅ 安全
  - `WebFetchTool`：HTTP 客户端是否线程安全（见 H-1）
  - `MCPTool`：JSON-RPC 客户端是否线程安全
- [ ] **K-1 修复**：`ToolExecutor::execute()` 是 `const` 但 `tool->call()` 可能修改工具状态
  - 方案 A：`ITool::call()` 标注 `const` 语义为"逻辑 const"，工具内部用 mutex 保护可变状态
  - 方案 B：`ToolExecutor` 为每个并行 tool_use 创建工具副本（若工具支持 clone）
  - 方案 C：限制并行执行只针对无状态工具，有状态工具串行
- [ ] **决策记录**：在 `plan/` 下产出《工具线程安全审计报告》，明确每个工具的并行策略
- [ ] **日志（G-1）**：工具 execute 入口/出口、权限拒绝、异常捕获处加 LOG

### Phase 3.5：ReAct Agent 增强（3 天）— 原 Phase 3
> **前置条件**：Phase 3 工具线程安全审计完成
- [ ] **3.1** 实现并行工具执行（`std::async`）— 仅对 Phase 3 审计通过的工具并行
- [ ] **3.2** 实现 `IReActObserver`，重构 `ChatSession` 回调
- [ ] **3.3** 实现 `ContextCompressor`（基础版：保留最近 N 轮 + 截断旧 tool_result）
- [ ] **3.4** 实现 `ToolExecutor` 结果截断（`was_truncated` 字段）
- [ ] **T-3 扩展**：`ChatRenderer` 跨线程字段原子化（`m_spinner_active`/`m_viewing_thinking`/`m_total_tokens` 等）
- [ ] 编译验证 + Agent 端到端测试
- [ ] **日志（G-1）**：ReActLoop 每轮 Thought/Action/Observation；ContextCompressor 压缩决策；工具截断事件

### Phase 4：事件系统重构（2 天）
- [ ] **1.1** 新增 `IEventBus` 接口
- [ ] `EventBus` 继承 `IEventBus`
- [ ] `ChatSession` 构造函数注入 `IEventBus&`
- [ ] `TaskManager` 注入 `IEventBus&`
- [ ] `main.cpp` 组装时显式注入
- [ ] **1.3** 新建 `core/events/events.h`，迁移事件类型
- [ ] **C-1 扩展**：`ConfigManager` 单例 DI 化（同 EventBus 模式）
  - 抽取 `IConfigManager` 接口
  - `ChatSession` / `Terminal` / `SetupWizard` / `ModelSelector` 等接收 `IConfigManager&`
  - 测试用 `MockConfigManager` 注入
- [ ] **T-1 扩展**：修复 `EventBus::publish()` 持锁调回调的重入死锁风险
  - 方案：回调执行移出锁外（拷贝订阅者列表后释放锁再调用）
- [ ] **日志（G-1）**：subscribe/unsubscribe、publish 同步/异步、队列积压告警

### Phase 5：CMake 模块化（1 天）
- [ ] **4.1** 按模块拆分 `CMakeLists.txt`
- [ ] 验证所有平台可编译
- [ ] **日志（G-1）**：无（构建系统无运行时日志需求）

### Phase 6：回归测试与长期技术债（3 天）
- [ ] 全量单元测试通过
- [ ] **X-2 修复**：集成测试 Python server 自动化启动（CMake fixture 或 test 启动脚本）
- [ ] 集成测试通过（需 LLM backend）
- [ ] 性能基准：对比重构前后 token 吞吐、工具调用延迟、并行 vs 串行工具执行
- [ ] 内存检查：无泄漏、无越界
- [ ] **长期技术债登记**（不在本方案实施，登记到 PLAN_PHASE4 或新文档）：
  - L-1/L-2/L-3：raw pointer 系统性替换为 `std::reference_wrapper` 或 `not_null<T*>`
  - H-1/H-2/H-3：HTTP 客户端连接池、总时长超时、重试逻辑统一
  - E-5/E-6：ExecutionResult 字段语义、HttpResponse 错误码（移交 v2）
- [ ] **日志（G-1）**：补齐遗漏模块（PermissionChecker / SSEStreamReader 等）

---

## 六、风险与缓解

| 风险 | 影响 | 缓解措施 |
|------|------|---------|
| 并行工具执行导致文件竞争 | 高 | Phase 3 审计先行；LLM 同一轮 tool_use 通常不操作同一文件；如有，由 OS 文件锁保护；后续可加文件锁管理器 |
| **工具非线程安全导致并行化失败**（T-2/K-1） | 高 | Phase 3 审计后，对不安全工具降级为串行执行（方案 C）；审计报告明确每个工具策略 |
| **非 TUI 场景异步事件滞留**（K-2） | 中 | Phase 1 扩展：Client 内部启动泵线程或同步消费；测试用 MockEventBus 同步发布 |
| 上下文压缩导致 LLM 丢失关键信息 | 中 | 保留最近 3 轮完整上下文，仅压缩更早的；添加日志记录被压缩内容 |
| ThreadPool 引入死锁 | 中 | 线程池任务不持有锁；Task 执行完自动通知；使用 `std::future::wait_for` 超时 |
| IEventBus/IConfigManager/ITaskManager 接口变更导致编译失败 | 中 | 渐进式迁移，保留旧接口做 deprecated 转发；每次只改一个模块；Phase 0.5 测试保护 |
| **EventBus publish 重入死锁**（T-1） | 中 | Phase 4 修复：回调执行移出锁外；Phase 0.5 测试覆盖重入场景 |
| **ChatRenderer 跨线程字段撕裂**（T-3） | 中 | Phase 3.5 原子化；Phase 0.5 测试覆盖并发渲染 |
| CMake 拆分破坏构建 | 低 | 保留根 CMake 做 fallback；CI 验证 Windows/Linux 双平台 |
| **重构无回归网**（X-1） | 高 | Phase 0.5 先行补测试；每个 Phase 完成后跑全量测试 |

---

## 七、验收标准

### 7.1 功能验收
- [ ] `TaskManager::update()` 每帧被调用（TUI 与非 TUI 场景均覆盖），已完成任务正确清理
- [ ] 并发执行 10 个无状态 `BashTool`（`sleep 1`），总耗时 < 2s（验证并行）
- [ ] 长会话（>30 轮）不触发 context limit 错误（验证压缩）
- [ ] Ctrl+C 可中断正在执行的工具（验证取消信号传递）

### 7.2 架构验收
- [ ] `grep -r "EventBus::instance()" src/` 仅在 `core/events/` 和 `main.cpp` 中出现
- [ ] `grep -r "ConfigManager::instance()" src/` 仅在 `core/config/` 和 `main.cpp` 中出现
- [ ] `grep -r "TaskManager::instance()" src/` 仅在 `core/task/` 和 `main.cpp` 中出现
- [ ] 三个单例均有对应接口（IEventBus / IConfigManager / ITaskManager）+ Mock 实现

### 7.3 质量验收
- [ ] 单元测试全部通过（`build/bin/workx_unit_tests.exe`），覆盖率较 Phase 0.5 前提升
- [ ] 集成测试可自动化运行（X-2 修复）
- [ ] 无新增编译警告
- [ ] **日志覆盖**：核心模块（EventBus/TaskManager/ReActLoop/ChatSession/ToolExecutor/HttpClient）均有 LOG 埋点，关键路径可追溯

### 7.4 文档验收
- [ ] `plan/工具线程安全审计报告.md` 产出（Phase 3）
- [ ] 本文档各 Phase 完成后勾选并标注实际耗时

---

## 八、关联项目

### 8.1 错误处理统一（独立立项 — V2）

**项目代号**：`ARCH_REFACTOR_PLAN_V2.md`（待创建）
**启动时机**：本方案（V1）完成后启动
**覆盖问题**：
- E-1：4 种错误处理风格并存（Result<T,E> / struct+bool / 异常 / HttpResponse+string）
- E-2：`Result::unwrap()` 抛异常反模式
- E-3：`ChatSession::send_message()` 返回 void，同步调用方无法感知失败
- E-5：`ExecutionResult` 字段语义重叠（移交 V2）
- E-6：`HttpResponse` 无错误码分类（移交 V2）

**设计方向（初稿）**：
- 统一采用 `Result<T, Error>` 风格，Error 为带错误码 + 上下文的类型
- 废弃 `unwrap()` 抛异常路径，改为 `get_or` / `map` / `and_then` 链式
- `send_message()` 返回 `Result<ReActResult, Error>`
- `ExecutionResult` 重构为 `Result<ToolResult, ToolError>`，`was_truncated` 并入 ToolResult

### 8.2 长期技术债（登记到 PLAN_PHASE4 或新文档）

- L-1/L-2/L-3：raw pointer 系统性替换
- L-5：StreamSession::m_multi 生命周期
- T-4/T-5/T-6：StreamingBuffer / Terminal / Task 残余非原子字段
- C-2/C-3/C-4：配置 schema、环境变量文档、ConfigScope 落地
- G-2/G-3/G-4：Logger 析构 detach、命名空间、shared_ptr 单例
- H-1/H-2/H-3/H-4：HTTP 客户端连接池、超时、重试、URL 解析
- X-3：测试 Mock 增强
- D-2/D-3：DI 容器、IBackend 接口职责

---

*文档版本: 1.1（整合架构探索发现，调整执行顺序）*  
*基于代码版本: 2026-07-26*  
*关联计划: react-loop-plan.md (已完成), PLAN_PHASE4.md (正交), ARCH_REFACTOR_PLAN_V2.md (错误处理统一，待启动)*
