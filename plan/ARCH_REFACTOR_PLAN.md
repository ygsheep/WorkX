# WorkX 架构重构方案 — 事件驱动 + 任务系统 + ReAct Agent

> **状态**: 待实施  
> **范围**: 聚焦三大子系统的架构级重构（非细节修 bug）  
> **前提**: ReActLoop（react_loop.h/cpp）已实现，本方案在其基础上做系统性改进  
> **目标**: 解耦单例依赖、修复任务系统缺陷、提升 Agent 并行能力与可观测性

---

## 0. 诊断总览

| 子系统 | 核心问题 | 严重度 | 现有计划覆盖？ |
|--------|---------|--------|-------------|
| **EventBus** | 全局单例、无自动消费机制、事件类型散落 | 🔴 P0 | ❌ 未覆盖 |
| **TaskManager** | `update()` 无人调用、无线程池、取消信号不传递到 Tool | 🔴 P0 | ❌ 未覆盖 |
| **ReActLoop** | 工具串行执行、无上下文压缩、Observation 无截断 | 🟠 P1 | ❌ 未覆盖 |
| **ChatSession** | 回调 lambda 过重、`session_id` 硬编码、重试递归 | 🟠 P1 | ⚠️ 部分覆盖（PHASE4） |
| **CMake/目录** | 源文件手动枚举、无子模块 CMake | 🟡 P2 | ❌ 未覆盖 |

**与现有计划的关系**：
- `react-loop-plan.md` ✅ 已完成（ReActLoop 提取）
- `PLAN_PHASE4.md` 🔧 待实施（代码质量 P2/P3，本方案不重复）
- 本方案 🆕 聚焦 **架构级** 问题，与 PHASE4 正交、可并行

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

### Phase 0：准备（1 天）
- [ ] 备份当前代码（git tag `before-arch-refactor`）
- [ ] 确认所有测试可编译运行
- [ ] 创建 feature 分支 `arch-refactor`

### Phase 1：P0 紧急修复（2 天）
- [ ] **1.2** 在 `Terminal::run()` 中加入 `process_async_events()` 和 `TaskManager::update()` 调用
- [ ] **2.1** 同上（已合并到 1.2）
- [ ] **2.3** 修复 `ToolContext` 取消信号：传递 `should_cancel` 引用
- [ ] **2.4** `TaskStatus` 改为原子类型
- [ ] 编译验证 + 运行基本测试

### Phase 2：任务系统增强（2 天）
- [ ] **2.2** 实现 `ThreadPool`，替换 `TaskManager` 裸线程
- [ ] `TaskManager` 适配线程池接口
- [ ] 压力测试：并发启动 20+ 个 Task，验证线程数受限

### Phase 3：ReAct Agent 增强（3 天）
- [ ] **3.1** 实现并行工具执行（`std::async`）
- [ ] **3.2** 实现 `IReActObserver`，重构 `ChatSession` 回调
- [ ] **3.3** 实现 `ContextCompressor`（基础版：保留最近 N 轮 + 截断旧 tool_result）
- [ ] **3.4** 实现 `ToolExecutor` 结果截断
- [ ] 编译验证 + Agent 端到端测试

### Phase 4：事件系统重构（2 天）
- [ ] **1.1** 新增 `IEventBus` 接口
- [ ] `EventBus` 继承 `IEventBus`
- [ ] `ChatSession` 构造函数注入 `IEventBus&`
- [ ] `TaskManager` 注入 `IEventBus&`
- [ ] `main.cpp` 组装时显式注入
- [ ] **1.3** 新建 `core/events/events.h`，迁移事件类型

### Phase 5：CMake 模块化（1 天）
- [ ] **4.1** 按模块拆分 `CMakeLists.txt`
- [ ] 验证所有平台可编译

### Phase 6：回归测试（2 天）
- [ ] 全量单元测试通过
- [ ] 集成测试通过（需 LLM backend）
- [ ] 性能基准：对比重构前后 token 吞吐、工具调用延迟
- [ ] 内存检查：无泄漏、无越界

---

## 六、风险与缓解

| 风险 | 影响 | 缓解措施 |
|------|------|---------|
| 并行工具执行导致文件竞争 | 高 | LLM 同一轮 tool_use 通常不操作同一文件；如有，由 OS 文件锁保护；后续可加文件锁管理器 |
| 上下文压缩导致 LLM 丢失关键信息 | 中 | 保留最近 3 轮完整上下文，仅压缩更早的；添加日志记录被压缩内容 |
| ThreadPool 引入死锁 | 中 | 线程池任务不持有锁；Task 执行完自动通知；使用 `std::future::wait_for` 超时 |
| IEventBus 接口变更导致编译失败 | 低 | 渐进式迁移，保留旧接口做 deprecated 转发；每次只改一个模块 |
| CMake 拆分破坏构建 | 低 | 保留根 CMake 做 fallback；CI 验证 Windows/Linux 双平台 |

---

## 七、验收标准

- [ ] `TaskManager::update()` 每帧被调用，已完成任务正确清理
- [ ] 并发执行 10 个 `BashTool`（`sleep 1`），总耗时 < 2s（验证并行）
- [ ] 长会话（>30 轮）不触发 context limit 错误（验证压缩）
- [ ] `grep -r "EventBus::instance()" src/` 仅在 `core/events/` 和 `main.cpp` 中出现
- [ ] 单元测试全部通过（`build/bin/workx_unit_tests.exe`）
- [ ] 无新增编译警告

---

*文档版本: 1.0*  
*基于代码版本: 2026-07-26*  
*关联计划: react-loop-plan.md (已完成), PLAN_PHASE4.md (正交)*
