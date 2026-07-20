# WorkX 修复 Plan — 阶段 1：稳定性与安全（P0）

> **状态**：待审批（未做任何代码修改）
> **范围**：仅修复 P0 级问题，P1/P2 留待后续阶段
> **依据**：[CODE_REVIEW_REPORT.md](file:///c:\Users\young\Desktop\Develop\WorkX\CODE_REVIEW_REPORT.md) 第 9 节
> **用户决策**：
>   - 14 个 stub 模块暂时不实现（permission/compact/prompt/util 等）
>   - `process_bash_command` 不修复，仅加 TODO 注释（属于 P1，留待后续阶段处理）

---

## 0. 阶段 1 修复范围

| 类别 | 数量 | 涉及文件 |
|------|------|---------|
| 并发与生命周期 | 12 项 | chat_session.{h,cpp}、client.{h,cpp}、event_bus.h、task_manager.{h,cpp} |
| 数据完整性 | 4 项 | client.cpp、FileWriteTool/file_write_tool.cpp、FileWriteTool/diff.cpp |
| 安全 | 2 项 | sse_parser.cpp、remote_backend.cpp |
| 工具执行 | 4 项 | tool/executor.h、FileReadTool/file_read_tool.cpp、GlobTool/glob_tool.cpp |
| 主流程 | 3 项 | app/main.cpp |
| **合计** | **25 项** | **13 个文件** |

> 渲染相关 P0（6.1-6.5、6.16、7.1）留待阶段 2
> ChatSession regenerate 未检查 generating（2.4）合并到 ChatSession 并发修复

---

## 1. ChatSession 并发与生命周期修复（5 项 P0）

### 1.1 问题清单
- **2.1** `m_messages`/`m_system_prompt`/`m_tool_registry` 多线程无同步
- **2.2** 后台 lambda 捕获 this，析构不等待 Task 完成
- **2.3** agent 循环 lambda 无 try-catch，异常逃逸致 `m_generating` 永不复位
- **2.4** `regenerate()` 未检查 `m_generating`
- **2.5** 递归重试时 `m_generating` 错误重置（合并修复）

### 1.2 修复方案

#### 1.2.1 引入 mutex 保护共享状态（[chat_session.h](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\core\chat_session.h)）

在 ChatSession 私有成员中新增：

```cpp
private:
    mutable std::mutex m_state_mutex;  // 保护 m_messages、m_system_prompt、m_tool_registry
    std::shared_ptr<Task> m_current_task;  // 跟踪当前后台任务，用于析构等待
    std::condition_variable m_task_cv;  // 任务完成通知
```

#### 1.2.2 所有共享状态访问加锁（[chat_session.cpp](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\core\chat_session.cpp)）

**修改的方法**：
- `set_system_prompt` / `set_tool_registry`：写锁
- `clear_history` / `regenerate` / `get_messages` / `save_session` / `load_session`：写/读锁
- `build_request`：读锁（拷贝 m_messages、m_system_prompt、m_tool_registry）
- `run_completion` lambda 内对 `m_messages` 的 push_back/pop_back：写锁

**关键修改点**：
- [chat_session.cpp:109](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\core\chat_session.cpp#L109) `run_completion` 入口的 push_back
- [chat_session.cpp:246, 285-288, 309, 347, 365-366, 407-408](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\core\chat_session.cpp#L246) lambda 内的 m_messages 修改
- [chat_session.cpp:64-66](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\core\chat_session.cpp#L64) build_request 的 m_messages 读取

#### 1.2.3 析构等待后台任务（2.2）

修改 `~ChatSession()`：

```cpp
ChatSession::~ChatSession() {
    unsubscribe_interrupt();
    if (m_provider) {
        m_provider->interrupt();
    }
    // 等待后台任务完成，防止 use-after-free
    std::shared_ptr<Task> task;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        task = m_current_task;
    }
    if (task) {
        task->cancel();
        // 等待任务结束（最长 30 秒兜底）
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (task->isRunning() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}
```

#### 1.2.4 agent 循环 try-catch 包裹（2.3）

在 lambda 体外层加 try-catch：

```cpp
auto task = TaskManager::instance().launch("completion",
    [this, ...](const std::atomic<bool>& should_cancel) {
        try {
            // 原有 agent 循环全部代码
        } catch (const std::exception& e) {
            EventBus::instance().publish_async(StreamErrorEvent{
                .session_id = "default",
                .message = std::format("Agent loop exception: {}", e.what()),
                .retryable = false
            });
        } catch (...) {
            EventBus::instance().publish_async(StreamErrorEvent{
                .session_id = "default",
                .message = "Agent loop unknown exception",
                .retryable = false
            });
        }
        // 确保 m_generating 一定复位
        m_generating.store(false);
        {
            std::lock_guard<std::mutex> lock(m_state_mutex);
            m_current_task.reset();
        }
        m_task_cv.notify_all();
    },
    TaskType::Normal
);
{
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_current_task = task;
}
```

#### 1.2.5 `regenerate()` 检查 `m_generating`（2.4）

```cpp
void ChatSession::regenerate() {
    if (m_generating.load()) {
        EventBus::instance().publish_async(StreamErrorEvent{
            .session_id = "default",
            .message = "Still generating, cannot regenerate",
            .retryable = true
        });
        return;
    }
    // 原有逻辑
}
```

#### 1.2.6 递归重试改为不立即 reset `m_generating`（2.5）

原代码在递归调用 `run_completion(user_text, retry_attempt + 1)` 后立即 `m_generating.store(false)`（[chat_session.cpp:168, 298](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\core\chat_session.cpp#L168)）。

**修复**：递归重试路径中，不重置 `m_generating`（由新 Task 的 lambda 在结束时统一重置）。仅在重试耗尽、不再启动新 Task 时才重置。

### 1.3 风险与缓解
- **死锁风险**：lambda 内同时持有 m_state_mutex 和调用 m_provider/EventBus。**缓解**：lambda 内尽量缩短锁持有时间，仅在 m_messages 修改时加锁，其他时候用局部变量。
- **性能影响**：每次 push_back 加锁。**缓解**：m_messages 修改频率低（每轮 agent loop 几次），可接受。
- **测试**：需手动测试「生成中 /clear」、「生成中 /regen」、「生成中 Ctrl+C 后立即发新消息」三种场景。

---

## 2. Client 并发与生命周期修复（4 项 P0）

### 2.1 问题清单
- **4.1** `chat_async` 双重包装回调导致 assistant 消息重复入历史
- **4.3** `m_messages` 完全无线程同步
- **4.4** `stream_chat_async` 的 generating 检查存在 TOCTOU
- **4.5** `stream_chat_async` 捕获 this，Client 析构后悬空

### 2.2 修复方案

#### 2.2.1 修复双重包装（4.1，[client.cpp:406-436](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\client.cpp#L406)）

`chat_async` 当前已经包装了 `on_token`/`on_done` 累积 content 并 push assistant 消息，然后调用 `stream_chat_async`，后者又包装一次。

**修复方案 A（推荐）**：让 `chat_async` 不包装，直接传原始 cbs 给 `stream_chat_async`，由后者统一处理。

```cpp
Result<void, std::string> Client::chat_async(const std::string& user_text,
                                              const ChatCallbacks& cbs) {
    // chat_async 不再包装，直接委托 stream_chat_async
    // stream_chat_async 已负责累积 content 并 push assistant 消息
    return stream_chat_async(user_text, cbs);
}
```

**修复方案 B**：让 `stream_chat_async` 检测是否已被包装（通过 flag），避免重复包装。复杂，不推荐。

#### 2.2.2 引入 mutex 保护 m_messages（4.3，[client.h:160](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\client.h#L160)）

```cpp
private:
    mutable std::mutex m_messages_mutex;  // 保护 m_messages
```

修改的方法：
- `build_request`：读锁
- `chat` / `stream_chat` / `stream_chat_async`：写锁（push user 消息）
- `chat_async` 包装的 on_done：写锁（push assistant）
- `clear_history` / `regenerate`：写锁
- `history`：读锁（**注意**：返回 const 引用会破坏线程安全，需改为返回拷贝或加锁访问）

**关键决策**：`history()` 当前返回 `const std::vector<ChatMessage>&`，无法加锁保护调用方使用。**修复**：改为返回拷贝 `std::vector<ChatMessage> history() const;`，或文档说明「调用方需自行保证不并发」。

#### 2.2.3 修复 TOCTOU（4.4，[client.cpp:440-447](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\client.cpp#L440)）

用 `compare_exchange_strong` 替代 load+store：

```cpp
bool expected = false;
if (!m_generating.compare_exchange_strong(expected, true)) {
    return Result<void, std::string>::err("Already generating");
}
```

#### 2.2.4 析构等待后台任务（4.5，[client.cpp:99-107](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\client.cpp#L99)）

参考 ChatSession 方案：

```cpp
Client::~Client() {
    if (m_subscribed && m_interrupt_token.is_valid()) {
        EventBus::instance().unsubscribe<InterruptEvent>(m_interrupt_token);
    }
    if (m_backend) {
        m_backend->interrupt();
    }
    // 等待后台任务完成
    // m_generating 会被 on_done/on_error 重置，等待其变 false
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (m_generating.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (m_backend) {
        m_backend->shutdown();
    }
}
```

### 2.3 风险与缓解
- **history() 接口变更**：返回拷贝会增加开销。**缓解**：调用方主要是 TUI 渲染，频率低，可接受。
- **死锁风险**：on_done 回调在后台线程持锁 push_back，主线程可能同时持锁。**缓解**：lock_guard 范围最小化。

---

## 3. EventBus 修复（3 项 P0）

### 3.1 问题清单
- **8.1** recursive_mutex 可能死锁
- **8.2** 非 std::exception 触发 terminate
- **8.3** publish_async lambda 捕获 this，单例销毁后 UB

### 3.2 修复方案（[event_bus.h](file:///c:\Users\young\Desktop\Develop\WorkX\src\core\events\event_bus.h)）

#### 3.2.1 publish 改用普通 mutex + 拷贝回调列表（8.1）

```cpp
template<typename T>
void publish(const T& event) {
    // 拷贝回调列表，避免持锁调用用户代码
    std::vector<CallbackWrapper> callbacks_copy;
    {
        std::lock_guard<std::mutex> lock(m_mutex);  // 改普通 mutex
        auto it = m_callbacks.find(typeid(T));
        if (it == m_callbacks.end()) return;
        callbacks_copy = it->second;
    }
    for (const auto& wrapper : callbacks_copy) {
        try {
            wrapper.callback(&event);
        } catch (const std::exception&) {
            // 吞标准异常
        } catch (...) {
            // 新增：吞所有异常，防止 terminate
        }
    }
}
```

**关键变更**：
1. `m_mutex` 从 `std::recursive_mutex` 改为 `std::mutex`
2. publish 先拷贝回调列表再释放锁，回调中可安全 subscribe/unsubscribe/publish
3. 新增 `catch (...)` 兜底（修复 8.2）

#### 3.2.2 publish_async 用 weak_ptr 避免悬空（8.3）

由于 EventBus 是单例，生命周期与进程一致，真正的问题是「单例销毁后异步队列仍在处理」。**修复**：

```cpp
template<typename T>
void publish_async(const T& event) {
    std::lock_guard<std::mutex> lock(m_async_mutex);
    // 拷贝 event 到 lambda，不捕获 this
    T event_copy = event;
    m_async_queue.push_back([event_copy = std::move(event_copy)]() {
        EventBus::instance().publish(event_copy);  // 通过 instance() 访问
    });
}
```

**说明**：原代码 `this->publish(event)` 改为 `EventBus::instance().publish(event_copy)`。若单例已销毁，`instance()` 返回的引用访问会 UB，但实际单例销毁发生在 `atexit` 阶段，此时异步队列也应已清空。**额外保险**：增加 `m_shutdown` 标志，`process_async_events` 在 shutdown 后不再处理。

#### 3.2.3 clear() 同时清异步队列（补充 8.11 部分）

```cpp
void clear() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_callbacks.clear();
    }
    std::lock_guard<std::mutex> lock(m_async_mutex);
    m_async_queue.clear();
}
```

### 3.3 风险与缓解
- **回调列表拷贝开销**：每次 publish 拷贝 vector。**缓解**：回调数量通常 < 10，可接受；高频事件（StreamTokenEvent）可考虑其他方案。
- **死锁彻底消除**：用户回调中可安全调用任何 EventBus 方法。

---

## 4. TaskManager 修复（3 项 P0）

### 4.1 问题清单
- **8.4** `start` 中 detach 线程
- **8.5** `Task::cancel` 状态机不完整
- **8.6** 析构不等待结束

### 4.2 修复方案（[task_manager.h](file:///c:\Users\young\Desktop\Develop\WorkX\src\core\task\task_manager.h)、[task_manager.cpp](file:///c:\Users\young\Desktop\Develop\WorkX\src\core\task\task_manager.cpp)）

#### 4.2.1 改 detach 为 join 管理（8.4）

新增线程句柄存储：

```cpp
private:
    struct TaskEntry {
        std::shared_ptr<Task> task;
        std::thread thread;
    };
    std::vector<TaskEntry> m_entries;
    mutable std::mutex m_tasks_mutex;
    std::condition_variable m_tasks_cv;  // 任务完成通知
```

修改 `start`：

```cpp
void TaskManager::start(std::shared_ptr<Task> task) {
    if (!task) return;
    if (task->getType() == TaskType::Blocking) {
        task->execute();
        return;
    }
    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    auto& entry = m_entries.emplace_back();
    entry.task = task;
    entry.thread = std::thread([task]() {
        task->execute();
    });
}
```

#### 4.2.2 修复 Task::cancel 状态机（8.5）

```cpp
void cancel() {
    m_should_cancel = true;
    // 不立即设 Cancelled，由 execute() 检测 should_cancel 后设
}
```

修改 `Task::execute`（已有逻辑正确，仅需确认）：

```cpp
void Task::execute() {
    // ...
    m_status = TaskStatus::Running;
    try {
        m_func(m_should_cancel);
        if (m_should_cancel) {
            m_status = TaskStatus::Cancelled;  // 这里设置
        } else {
            markCompleted();
        }
    } catch (...) { ... }
}
```

**关键**：`isFinished()` 在 Cancelled 时返回 true，但 task 真正结束要等 `execute()` 返回。`waitForAll` 需 join 线程确认。

#### 4.2.3 析构 join 所有线程（8.6）

```cpp
TaskManager::~TaskManager() {
    cancelAll();
    waitForAll();
    // join 所有线程
    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    for (auto& entry : m_entries) {
        if (entry.thread.joinable()) {
            entry.thread.join();
        }
    }
    m_entries.clear();
}
```

#### 4.2.4 waitForAll 改用 condition_variable

```cpp
void TaskManager::waitForAll() {
    std::unique_lock<std::mutex> lock(m_tasks_mutex);
    m_tasks_cv.wait(lock, [this]() {
        return std::all_of(m_entries.begin(), m_entries.end(),
            [](const TaskEntry& e) { return e.task->isFinished(); });
    });
}
```

并在任务完成时 notify（在 `Task::execute` 结束后或 `markCompleted`/`markFailed` 中）。

#### 4.2.5 update() 同时清理已 join 的线程

```cpp
void TaskManager::update() {
    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    for (auto it = m_entries.begin(); it != m_entries.end(); ) {
        if (it->task->isFinished() && it->task->getType() != TaskType::Critical) {
            if (it->thread.joinable()) it->thread.join();
            it = m_entries.erase(it);
        } else {
            ++it;
        }
    }
}
```

### 4.3 风险与缓解
- **线程 join 阻塞**：若 task 不响应 cancel，析构会卡住。**缓解**：waitForAll 已有 30 秒兜底（需补充）。
- **接口变更**：`getTasks()` 返回 `vector<shared_ptr<Task>>` 需调整为从 entries 提取。**缓解**：保持接口，内部转换。
- **测试**：需测试「启动长任务后立即退出」、「启动多个并发任务后 cancelAll」。

---

## 5. FileWriteTool 修复（2 项 P0）

### 5.1 问题清单
- **3.2** 写入失败导致数据丢失
- **3.6** `fs::exists` 失败绕过 pre-read 检查

### 5.2 修复方案（[file_write_tool.cpp:243-370](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\tool\FileWriteTool\file_write_tool.cpp#L243)）

#### 5.2.1 原子写入（3.2）

替换 [file_write_tool.cpp:321-336](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\tool\FileWriteTool\file_write_tool.cpp#L321) 的直接 trunc 写入：

```cpp
// 写临时文件 → fsync → 原子 rename
fs::path temp_path = file_path;
temp_path += ".tmp." + std::to_string(
    std::chrono::steady_clock::now().time_since_epoch().count());

{
    std::ofstream out(temp_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return ToolResult::error(
            std::format("Failed to create temp file: {}", temp_path.string()));
    }
    out << write_input.content;
    out.flush();
    if (!out) {
        std::error_code rm_ec;
        fs::remove(temp_path, rm_ec);  // 清理临时文件
        return ToolResult::error(
            std::format("Failed to write file: {}", write_input.file_path));
    }
    out.close();
}

// 原子 rename（Windows 上 fs::rename 会失败若目标存在，需先删目标）
std::error_code rename_ec;
fs::rename(temp_path, file_path, rename_ec);
if (rename_ec) {
    // rename 失败，回退：删除临时文件，尝试直接覆盖
    std::error_code rm_ec;
    fs::remove(temp_path, rm_ec);
    // 直接写入（保留原逻辑作为兜底）
    std::ofstream out(file_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        return ToolResult::error(
            std::format("Failed to write file (rename failed: {}): {}",
                        rename_ec.message(), write_input.file_path));
    }
    out << write_input.content;
    out.flush();
    out.close();
    if (!out) {
        return ToolResult::error(
            std::format("Failed to write file: {}", write_input.file_path));
    }
}
```

#### 5.2.2 修复 `fs::exists` 失败处理（3.6）

修改 [file_write_tool.cpp:288](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\tool\FileWriteTool\file_write_tool.cpp#L288)：

```cpp
std::error_code ec;
const bool exists = fs::exists(file_path, ec);
if (ec) {
    // exists 检查失败，无法判断是 create 还是 update
    // 安全起见：当作 update 处理（执行 pre-read 检查），若 pre-read 也失败再报错
    return ToolResult::error(
        std::format("Cannot determine file existence ({}): {}",
                    ec.message(), write_input.file_path));
}
const bool is_update = exists;
```

### 5.3 风险与缓解
- **Windows rename 行为**：Windows 上 `fs::rename` 目标存在时失败。**缓解**：上面代码已处理回退。
- **临时文件残留**：异常退出时可能残留 `.tmp.xxx` 文件。**缓解**：可接受，或在程序启动时清理。
- **测试**：需测试「写入失败时原文件完整性」、「并发写入同一路径」（虽然 Agent 通常单线程）。

---

## 6. LCS Diff 算法 OOM 修复（1 项 P0）

### 6.1 问题清单
- **3.3** LCS O(n*m) 内存，2MB 文件可 OOM 2.5GB

### 6.2 修复方案（[diff.cpp:60-77](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\tool\FileWriteTool\diff.cpp#L60)）

#### 方案选择：滚动数组优化 LCS（最小改动，立竿见影）

LCS 空间可优化到 O(min(m,n))，但需放弃回溯路径。**为了保留回溯能力且控制改动**，采用**两阶段方案**：

**阶段 A（本次实施）：限制 diff 输入大小 + 一维数组**

1. 在 `generate_line_diff` 入口加保护：

```cpp
std::vector<DiffLine> generate_line_diff(
    const std::string& old_content,
    const std::string& new_content
) {
    const auto old_lines = split_lines(old_content);
    const auto new_lines = split_lines(new_content);

    // 新增：行数过多时降级为"全替换"diff，避免 OOM
    const size_t MAX_DIFF_LINES = 5000;  // 约 2MB 文件、每行 400 字符
    if (old_lines.size() > MAX_DIFF_LINES || new_lines.size() > MAX_DIFF_LINES) {
        std::vector<DiffLine> diff;
        // 全部标记为 Remove + Add
        for (size_t i = 0; i < old_lines.size(); ++i) {
            diff.push_back({DiffOp::Remove, static_cast<int>(i + 1), 0, old_lines[i]});
        }
        for (size_t j = 0; j < new_lines.size(); ++j) {
            diff.push_back({DiffOp::Add, 0, static_cast<int>(j + 1), new_lines[j]});
        }
        return diff;
    }
    // ... 原逻辑
}
```

2. 将 `vector<vector<int>>` 改为一维 `vector<int>` + 手动索引（cache 友好）：

```cpp
std::vector<int> build_lcs_table_flat(
    const std::vector<std::string>& old_lines,
    const std::vector<std::string>& new_lines
) {
    const size_t m = old_lines.size();
    const size_t n = new_lines.size();
    std::vector<int> dp((m + 1) * (n + 1), 0);
    for (size_t i = 1; i <= m; ++i) {
        for (size_t j = 1; j <= n; ++j) {
            if (old_lines[i - 1] == new_lines[j - 1]) {
                dp[i * (n + 1) + j] = dp[(i - 1) * (n + 1) + (j - 1)] + 1;
            } else {
                dp[i * (n + 1) + j] = std::max(
                    dp[(i - 1) * (n + 1) + j],
                    dp[i * (n + 1) + (j - 1)]
                );
            }
        }
    }
    return dp;
}
```

`backtrack_diff` 对应调整索引访问。

**阶段 B（留待后续，不在本次 Plan）**：实现 Myers O(ND) 算法彻底解决。

### 6.3 风险与缓解
- **5000 行阈值**：大文件 diff 变为全替换，可读性差。**缓解**：FileReadTool 已限制 2MB，5000 行覆盖大部分场景；超过时返回全替换 diff 仍优于 OOM。
- **一维数组索引错误**：手动索引易错。**缓解**：单元测试覆盖。

---

## 7. ToolExecutor try-catch 修复（1 项 P0）

### 7.1 问题清单
- **3.1** `ToolExecutor::execute` 无 try-catch，工具抛异常崩溃整个 Agent

### 7.2 修复方案（[executor.h:87](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\tool\executor.h#L87)）

修改 [executor.h:86-90](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\tool\executor.h#L86)：

```cpp
// 5. 执行工具
try {
    exec_result.result = tool->call(input, ctx);
    exec_result.is_error = exec_result.result.is_error;
} catch (const nlohmann::json::exception& e) {
    exec_result.result = ToolResult::error(
        std::format("Tool '{}' JSON exception: {}", tool_name, e.what()));
    exec_result.is_error = true;
} catch (const std::filesystem::filesystem_error& e) {
    exec_result.result = ToolResult::error(
        std::format("Tool '{}' filesystem error: {}", tool_name, e.what()));
    exec_result.is_error = true;
} catch (const std::exception& e) {
    exec_result.result = ToolResult::error(
        std::format("Tool '{}' exception: {}", tool_name, e.what()));
    exec_result.is_error = true;
} catch (...) {
    exec_result.result = ToolResult::error(
        std::format("Tool '{}' unknown exception", tool_name));
    exec_result.is_error = true;
}
return exec_result;
```

### 7.3 风险与缓解
- **无风险**：纯防御性代码，不改变正常路径行为。

---

## 8. 工具 input.get<T>() try-catch 修复（1 项 P0）

### 8.1 问题清单
- **3.4** `FileReadTool/GlobTool::call` 中 `input.get<T>()` 无 try-catch

### 8.2 修复方案

#### FileReadTool（[file_read_tool.cpp:213](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\tool\FileReadTool\file_read_tool.cpp#L213)）

```cpp
FileReadInput read_input;
try {
    read_input = input.get<FileReadInput>();
} catch (const nlohmann::json::exception& e) {
    return ToolResult::error(std::format("Input parse failed: {}", e.what()));
}
```

#### GlobTool（[glob_tool.cpp:128](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\tool\GlobTool\glob_tool.cpp#L128)）

```cpp
GlobInput glob_input;
try {
    glob_input = input.get<GlobInput>();
} catch (const nlohmann::json::exception& e) {
    return ToolResult::error(std::format("Input parse failed: {}", e.what()));
}
```

### 8.3 风险与缓解
- **无风险**：与 FileWriteTool 已有的 try-catch 对齐。

---

## 9. 配置值绕过文件大小限制修复（1 项 P0）

### 9.1 问题清单
- **3.5** 配置负数绕过 2MB 限制、`max_lines=INT_MAX` 触发 bad_alloc

### 9.2 修复方案（[file_read_tool.cpp:217-226](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\tool\FileReadTool\file_read_tool.cpp#L217)）

```cpp
// 读取配置，校验范围
int cfg_max_size = agent::ConfigManager::instance().get_or<int>(
    agent::keys::FILE_READ_MAX_SIZE,
    static_cast<int>(constants::MAX_FILE_SIZE_BYTES)
);
if (cfg_max_size <= 0 || cfg_max_size > 100 * 1024 * 1024) {  // 上限 100MB
    cfg_max_size = static_cast<int>(constants::MAX_FILE_SIZE_BYTES);
}
const size_t max_file_size = static_cast<size_t>(cfg_max_size);

int max_lines = agent::ConfigManager::instance().get_or<int>(
    agent::keys::FILE_READ_MAX_LINES,
    constants::MAX_LINES_TO_READ
);
if (max_lines <= 0 || max_lines > 100000) {  // 上限 10 万行
    max_lines = constants::MAX_LINES_TO_READ;
}
```

### 9.3 风险与缓解
- **无风险**：纯防御性校验。

---

## 10. SSE Parser 日志泄露修复（1 项 P0）

### 10.1 问题清单
- **4.7** SSE parser 把全部响应内容记入 INFO 日志

### 10.2 修复方案（[sse_parser.cpp:117](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\sse_parser.cpp#L117)）

```cpp
// 修改前：
// LOG_INFO("SSEParser parsed event: {}", event.data);

// 修改后：只记录长度，不记录内容
LOG_INFO("SSEParser parsed event: data_len={}, has_event={}",
         event.data.size(), !event.event.empty());
```

### 10.3 风险与缓解
- **调试不便**：排查 SSE 问题时看不到内容。**缓解**：提供 `WORKX_DEBUG_SSE` 宏，编译时开启才记录完整内容。

---

## 11. list_models 日志泄露 x-api-key 修复（1 项 P0）

### 11.1 问题清单
- **4.8** `list_models` 日志未对 `x-api-key` 脱敏

### 11.2 修复方案（[remote_backend.cpp:175-178](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\remote\remote_backend.cpp#L175)）

```cpp
for (const auto& [k, v] : header_pairs) {
    // 对所有可能的认证头脱敏
    std::string val;
    std::string k_lower = k;
    std::transform(k_lower.begin(), k_lower.end(), k_lower.begin(), ::tolower);
    if (k_lower == "authorization" || k_lower == "x-api-key" ||
        k_lower == "x-goog-api-key" || k_lower.find("key") != std::string::npos ||
        k_lower.find("token") != std::string::npos) {
        val = "***";
    } else {
        val = v;
    }
    LOG_INFO("[debug/models]  {}:{} ", k, val);
}
```

### 11.3 风险与缓解
- **无风险**：纯日志脱敏。

---

## 12. main.cpp 修复（2 项 P0）

### 12.1 问题清单
- **8.7** TaskManager::cancelAll 调用顺序错误
- **8.8** initialize 失败未调用 restore

### 12.2 修复方案

#### 12.2.1 修复 cancelAll 调用顺序（8.7，[main.cpp:451-456](file:///c:\Users\young\Desktop\Develop\WorkX\src\app\main.cpp#L451)）

```cpp
// ---- 清理 ----
EventBus::instance().unsubscribe<UserInputEvent>(input_token);
EventBus::instance().unsubscribe<ShutdownEvent>(shutdown_token);

// 先取消并等待所有任务，再恢复终端
// 这样任务完成时发的 UI 事件还能正常处理
TaskManager::instance().cancelAll();
TaskManager::instance().waitForAll();

// 清空 EventBus 订阅，防止后续异步事件触发已失效的回调
EventBus::instance().clear();

// 最后恢复终端
terminal.restore();  // 若原代码在 run() 返回后自动 restore，需确认
```

**说明**：原代码顺序是 `terminal.run()` 返回 → unsubscribe → `cancelAll` → `waitForAll`。需确认 `terminal.run()` 是否已 restore。若已 restore，则需调整为：unsubscribe → cancelAll → waitForAll → clear EventBus → restore。

需先读取 `terminal.run()` 实现确认 restore 时机，再定具体顺序。**Plan 标记**：执行阶段需先验证。

#### 12.2.2 修复 initialize 失败未 restore（8.8，[main.cpp:127-132](file:///c:\Users\young\Desktop\Develop\WorkX\src\app\main.cpp#L127)）

```cpp
auto init_result = terminal.initialize();
if (init_result.isErr()) {
    std::cerr << "Failed to initialize terminal: " << init_result.error() << "\n";
    terminal.restore();  // 新增：确保终端恢复
    return 1;
}
```

### 12.3 风险与缓解
- **顺序调整可能引入新问题**：cancelAll 触发的 UI 事件可能访问已 unsubscribe 的状态。**缓解**：unsubscribe 后 cancelAll，事件即使触发也无订阅者；waitForAll 后 clear EventBus。
- **terminal.restore() 重复调用**：需确认 restore 是否幂等。**缓解**：执行阶段验证。

---

## 13. 重试未清空输出修复（1 项 P0）

### 13.1 问题清单
- **4.2** 重试时未清空 `content_out`/`reasoning_out`，内容拼接错乱

### 13.2 修复方案（[client.cpp:188](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\client.cpp#L188)）

`run_stream` 的 for 循环开头已有 `content_out.clear()`（[client.cpp:185-186](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\client.cpp#L185)），但那是函数入口清空，重试时（循环回到顶部）**不会再次执行**。

**修复**：在 for 循环体内、每次 attempt 开始时清空：

```cpp
for (int attempt = 0; attempt <= m_max_retries; ++attempt) {
    // 新增：每次重试清空累积输出
    content_out.clear();
    reasoning_out.clear();

    if (should_stop()) {
        return Result<void, std::string>::ok();
    }
    // ... 原逻辑
}
```

并删除函数入口的 `content_out.clear(); reasoning_out.clear();`（已移到循环内）。

### 13.3 风险与缓解
- **无风险**：纯逻辑修复。

---

## 14. 阶段 1 实施顺序

建议按依赖关系分批实施，每批可独立编译验证：

### 批次 1：基础设施（无依赖）
1. **EventBus 修复**（第 3 节）— mutex 改普通、publish 拷贝列表、catch(...) 兜底、clear 清队列
2. **TaskManager 修复**（第 4 节）— 改 join、修复 cancel 状态机、waitForAll 用 cv

### 批次 2：工具执行（依赖批次 1）
3. **ToolExecutor try-catch**（第 7 节）
4. **FileReadTool/GlobTool input.get<T>() try-catch**（第 8 节）
5. **FileReadTool 配置值校验**（第 9 节）
6. **FileWriteTool 原子写入 + fs::exists 修复**（第 5 节）
7. **LCS diff OOM 修复**（第 6 节）

### 批次 3：会话层（依赖批次 1）
8. **ChatSession 并发与生命周期**（第 1 节）
9. **Client 并发与生命周期**（第 2 节）
10. **Client 重试未清空输出**（第 13 节）

### 批次 4：日志与主流程（依赖批次 1-3）
11. **SSE parser 日志脱敏**（第 10 节）
12. **list_models 日志脱敏**（第 11 节）
13. **main.cpp 顺序与 restore 修复**（第 12 节）

### 批次 5：验证
14. 编译通过
15. 运行 `build/bin/Debug/example_code_highlight.exe` 验证渲染未受影响
16. 手动测试：发起对话、`/clear`、`/regen`、Ctrl+C、退出
17. 手动测试：FileWrite 写入大文件、FileRead 读取大文件

---

## 15. 验证清单

阶段 1 完成后需验证：

- [ ] 编译无 warning（MSVC /W4）
- [ ] `example_code_highlight.exe` 运行正常（渲染层未改，应无影响）
- [ ] 单元测试全部通过
- [ ] 手动测试：发起对话 → 流式响应正常
- [ ] 手动测试：生成中 `/clear` → 不崩溃，历史清空
- [ ] 手动测试：生成中 `/regen` → 拒绝并提示
- [ ] 手动测试：生成中 Ctrl+C → 中断正常
- [ ] 手动测试：生成中立即退出 → 不崩溃
- [ ] 手动测试：FileWrite 写入失败（只读目录）→ 原文件不变
- [ ] 手动测试：FileWrite 写入 5000+ 行文件 → 不 OOM
- [ ] 手动测试：FileRead 配置负数 → 回退默认值
- [ ] 手动测试：日志中无 API Key、无响应内容
- [ ] 手动测试：异常退出后终端恢复正常模式

---

## 16. 待审批事项

请审阅本 Plan 并确认：

1. **修复方案是否合理？** 有无更好的替代方案？
2. **批次顺序是否合适？** 是否需要调整？
3. **LCS 5000 行阈值是否合理？** 可调整为其他值
4. **`history()` 接口变更**（返回拷贝）是否可接受？
5. **是否进入执行阶段？**

待你审批后，我将按批次顺序执行修改，每批完成后可独立验证。整个阶段 1 预计涉及 13 个文件的修改，无新增文件。
