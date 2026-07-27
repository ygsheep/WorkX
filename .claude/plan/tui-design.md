# Workx TUI 实现计划

## Context

基于 llama.cpp 的 `llama-cli` TUI 分析（`plan/llama-cli-tui-analysis.md`），为本项目设计一个 **考虑未来 Agent core 集成** 的终端聊天客户端。核心目标：TUI 作为纯渲染层，通过 EventBus 与逻辑层解耦；Agent core 未来可以无缝插入，TUI 代码零修改。

---

## 1. 分层架构

```
┌─────────────────────────────────────────────────────┐
│                    用户终端                           │
│              (ANSI escapes / Win32 API)               │
└──────────────────────────┬──────────────────────────┘
                           │
┌──────────────────────────▼──────────────────────────┐
│              TUI 层（纯渲染）                          │
│                                                      │
│  Terminal ── LineEditor ── ChatRenderer ── Spinner   │
│  History   ── ColorScheme  ── IPlatform (平台抽象)    │
│                                                      │
│  订阅: StreamToken*, AgentStep*, ErrorEvent          │
│  发布: UserInputEvent, CommandEvent                  │
└──────────────────────────┬──────────────────────────┘
                           │ EventBus
┌──────────────────────────▼──────────────────────────┐
│              Session 层（编排）                        │
│                                                      │
│  ChatSession (对话状态机) ── CommandRouter (斜杠命令) │
│                                                      │
│  订阅: UserInputEvent, CommandEvent                  │
│  发布: StreamTokenEvent, StreamDoneEvent, ErrorEvent │
└──────────────────────────┬──────────────────────────┘
                           │ EventBus
┌──────────────────────────▼──────────────────────────┐
│           [未来] Agent Core 层                        │
│                                                      │
│  AgentOrchestrator ── ToolRegistry ── PlanExecutor   │
│                                                      │
│  拦截: StreamDoneEvent (决定下一步)                   │
│  发布: AgentStepEvent, ToolCallEvent, AgentDoneEvent │
└──────────────────────────┬──────────────────────────┘
                           │ EventBus
┌──────────────────────────▼──────────────────────────┐
│              Backend 层（推理）                        │
│                                                      │
│  IBackend (接口) ── BackendFactory                   │
│    ├── RemoteBackend (OpenAI HTTP/SSE)               │
│    └── LocalBackend  (llama.cpp, 可选)               │
└─────────────────────────────────────────────────────┘
```

**关键设计决策**：ChatSession 依赖 `ICompletionProvider` 接口而非具体 Backend。`IBackend` 和 `IAgentCore` 都实现此接口。切换 Agent 模式只改 main.cpp 的组装方式，TUI/Backend 代码不变。

---

## 2. Agent 集成方式

**无 Agent（Phase 1-4）：**
```
main.cpp:
  auto backend = BackendFactory::create(config);
  auto session = ChatSession(std::move(backend));
  // ChatSession 直接调用 backend->submit()
```

**有 Agent（Phase 6+）：**
```
main.cpp:
  auto backend = BackendFactory::create(config);
  auto agent  = AgentOrchestrator(std::move(backend));
  agent.register_tool(std::make_unique<FileReadTool>());
  auto session = ChatSession(std::move(agent));
  // ChatSession 委托给 agent->process()
  // Agent 内部多次调用 IBackend，发出相同的事件 + 额外的 Agent 事件
```

**统一接口 ICompletionProvider：**
```cpp
class ICompletionProvider {
public:
    virtual ~ICompletionProvider() = default;
    virtual void submit_completion(const CompletionRequest& request) = 0;
    virtual void interrupt() = 0;
    virtual bool is_generating() const = 0;
};
```

IBackend（通过适配器）和 IAgentCore 都实现此接口。ChatSession 只依赖 ICompletionProvider。

---

## 3. 事件系统设计

所有层间通信通过 `mydev::EventBus`。

### 3.1 用户事件（TUI → Session）

| 事件 | 字段 | 说明 |
|------|------|------|
| `UserInputEvent` | `text`, `attachments` | 用户提交文本消息 |
| `CommandEvent` | `name`, `args` | 斜杠命令（/exit, /clear, /regen 等） |
| `InterruptEvent` | `force` | Ctrl+C 中断请求 |

### 3.2 流式事件（Backend/Agent → TUI）

| 事件 | 字段 | 说明 |
|------|------|------|
| `StreamTokenEvent` | `session_id`, `content_delta`, `reasoning_delta`, `is_thinking`, `token_count` | 单个流式 token |
| `StreamProgressEvent` | `session_id`, `total`, `processed` | Prompt 处理进度 |
| `StreamDoneEvent` | `session_id`, `full_content`, `full_reasoning`, `was_interrupted`, `prompt_tokens`, `generated_tokens`, `prompt_ms`, `generation_ms` | 流式完成 |
| `StreamErrorEvent` | `session_id`, `message`, `retryable` | 推理错误 |

### 3.3 Agent 事件（Agent → TUI，未来）

| 事件 | 字段 | 说明 |
|------|------|------|
| `AgentStepEvent` | `step_id`, `step_number`, `description` | Agent 推理步骤 |
| `ToolCallEvent` | `tool_name`, `arguments`, `call_id` | Agent 调用工具 |
| `ToolResultEvent` | `call_id`, `result`, `is_error` | 工具返回结果 |
| `AgentDoneEvent` | `final_response`, `total_steps`, `total_tool_calls`, `total_duration_ms` | Agent 编排完成 |

### 3.4 系统事件（跨切面）

| 事件 | 字段 | 说明 |
|------|------|------|
| `ModelLoadEvent` | `model_name`, `progress`, `complete`, `error` | 模型加载进度 |
| `BackendStatusEvent` | `status`, `backend_name`, `error` | 后端连接状态 |
| `ShutdownEvent` | `force` | 应用关闭请求 |

### 3.5 事件流

**简单聊天：**
```
UserInputEvent → ChatSession → IBackend → StreamTokenEvent* → StreamDoneEvent → TUI 渲染
```

**Agent 模式（未来）：**
```
UserInputEvent → AgentOrchestrator
  → AgentStepEvent [TUI 显示 "分析中..."]
  → IBackend → StreamTokenEvent* → StreamDoneEvent
  → ToolCallEvent [TUI 显示工具调用]
  → ToolResultEvent
  → AgentStepEvent [下一步]
  → IBackend → StreamTokenEvent* → StreamDoneEvent
  → AgentDoneEvent [TUI 显示最终结果]
```

---

## 4. 关键接口

### 4.1 IBackend

```cpp
class IBackend {
public:
    virtual ~IBackend() = default;
    virtual std::string name() const = 0;
    virtual Result<void, std::string> initialize(const BackendConfig& config) = 0;
    virtual void shutdown() = 0;
    virtual std::unique_ptr<IStreamReader> submit(const CompletionRequest& request) = 0;
    virtual bool is_ready() const = 0;
    virtual ModelInfo get_model_info() const = 0;
};
```

### 4.2 IStreamReader

```cpp
enum class StreamState { HasData, Complete, Error, Cancelled };

class IStreamReader {
public:
    virtual ~IStreamReader() = default;
    virtual StreamState next(std::function<bool()> should_stop, StreamChunk& out) = 0;
    virtual void cancel() = 0;
};
```

### 4.3 数据类型

```cpp
struct ChatMessage {
    enum Role { System, User, Assistant, Tool };
    Role role;
    std::string content;
    std::string reasoning_content;
};

struct CompletionRequest {
    std::vector<ChatMessage> messages;
    std::vector<std::string> stop_words;
    int32_t max_tokens = -1;
    float temperature = 0.8f;
    float top_p = 0.95f;
    bool stream = true;
};

struct StreamChunk {
    std::string content_delta;
    std::string reasoning_delta;
    int32_t token_count = 0;
    bool is_final = false;
    // timing info (valid when is_final)
    int32_t prompt_tokens = 0;
    int32_t generated_tokens = 0;
    double prompt_ms = 0.0;
    double generation_ms = 0.0;
};

struct BackendConfig {
    enum Type { Local, Remote };
    Type type;
    std::string model_path;     // Local
    int n_ctx = 4096;           // Local
    int n_gpu_layers = -1;      // Local
    std::string base_url;       // Remote
    std::string api_key;        // Remote
    std::string model_name;     // Remote
    int timeout_ms = 30000;     // Remote
};
```

---

## 5. Terminal 平台抽象

**决策：自建 Terminal 类，参考 llama.cpp 但面向 EventBus 设计**

| 特性 | llama.cpp console | workx::Terminal |
|------|-------------------|-----------------|
| 平台抽象 | namespace + #ifdef | class + IPlatform Strategy |
| 行编辑 | 内联在 namespace | LineEditor 类（可测试） |
| 颜色系统 | 6 个固定 display_type | ColorRole 枚举（可扩展） |
| 输出 | printf 到 stdout | render() 方法 + EventBus |
| 输入 | 阻塞 readline | 异步输入 + 回调 |
| Spinner | 独立线程 | 集成到渲染循环 |

```cpp
class Terminal {
public:
    struct Config {
        bool simple_io = false;
        bool use_color = true;
        std::string prompt_string = "> ";
        bool multiline_input = false;
    };
    explicit Terminal(const Config& config);
    Result<void, std::string> initialize();
    void restore();
    void run();       // 主输入循环
    void shutdown();

    // 渲染 API
    void set_color(ColorRole role);
    void reset_color();
    void write(std::string_view text);
    void spinner_start(std::string_view msg);
    void spinner_stop();

    // 回调
    using InputCallback = std::function<void(const std::string&)>;
    void set_input_callback(InputCallback cb);
    void set_completion_callback(CompletionCallback cb);
};

enum class ColorRole {
    Default, Prompt, UserInput, Assistant, Reasoning,
    System, Error, Command, ToolName, ToolOutput, Progress
};
```

**IPlatform 内部接口**（Strategy 模式，隔离平台代码）：
```cpp
class IPlatform {
    virtual Result<void, std::string> enable_raw_mode() = 0;
    virtual void disable_raw_mode() = 0;
    virtual int read_char() = 0;
    virtual void write_output(std::string_view text) = 0;
    virtual void move_cursor(int cols) = 0;
    virtual void clear_to_end_of_line() = 0;
    virtual int get_terminal_width() = 0;
};
// platform_posix.cpp — termios 实现
// platform_win32.cpp — Win32 Console API 实现
```

---

## 6. 线程模型

```
┌──────────────────┐   ┌──────────────────────┐   ┌────────────────┐
│   Main Thread    │   │   Worker Thread      │   │   Curl Thread  │
│                  │   │   (TaskManager)       │   │   (libcurl)    │
│ Terminal::run()  │   │                      │   │                │
│   ↓              │   │ ChatSession::        │   │ HTTP GET/POST  │
│ Read input       │   │   run_completion()   │   │ SSE streaming  │
│   ↓              │   │   ↓                  │   │   ↓            │
│ Publish          │   │ IStreamReader::next()│◄───│ SSEParser      │
│ UserInputEvent   │   │   ↓                  │   │ feed chunks    │
│   ↓              │   │ publish_async()      │   │                │
│ process_async_   │   │ StreamTokenEvent     │   │                │
│ events()         │   │                      │   │                │
│   ↓              │   │                      │   │                │
│ ChatRenderer     │   │                      │   │                │
│ → Terminal::write│   │                      │   │                │
└──────────────────┘   └──────────────────────┘   └────────────────┘
```

- 主线程：Terminal 输入 + EventBus::process_async_events() + ChatRenderer 输出
- Worker 线程：ChatSession::run_completion() 阻塞在 IStreamReader::next()
- **避免 llama.cpp 的坑**：不在推理线程调 console::log()，所有 UI 输出在主线程

---

## 7. 文件结构

```
workx/
├── CMakeLists.txt
├── vcpkg.json
├── src/
│   ├── main.cpp                         # 入口：解析参数，组装各层，运行
│   ├── core/                            # 基础组件（复制自 mydev）
│   │   ├── result.h
│   │   ├── event_bus.h
│   │   ├── task_manager.h / .cpp
│   │   ├── task_events.h
│   │   ├── sse_parser.hpp / .cpp
│   │   └── config_manager.h / .cpp
│   ├── tui/                             # TUI 层
│   │   ├── terminal.h / .cpp            # Terminal 类
│   │   ├── line_editor.h / .cpp         # 行编辑器
│   │   ├── color_scheme.h / .cpp        # 颜色方案
│   │   ├── chat_renderer.h / .cpp       # 事件订阅 + 渲染
│   │   ├── spinner.h / .cpp             # 加载动画
│   │   ├── history.h / .cpp             # 输入历史
│   │   └── platform/
│   │       ├── i_platform.h             # 平台接口
│   │       ├── platform_posix.cpp
│   │       └── platform_win32.cpp
│   ├── session/                         # Session 层
│   │   ├── chat_session.h / .cpp        # 对话状态机
│   │   ├── command_router.h / .cpp      # 斜杠命令路由
│   │   └── events.h                     # 所有事件定义
│   ├── backend/                         # Backend 层
│   │   ├── i_backend.h                  # IBackend 接口
│   │   ├── i_stream_reader.h            # IStreamReader 接口
│   │   ├── i_completion_provider.h      # 统一接口
│   │   ├── backend_config.h
│   │   ├── backend_factory.h / .cpp
│   │   ├── chat_types.h                 # ChatMessage, CompletionRequest, StreamChunk
│   │   ├── model_info.h
│   │   ├── remote/
│   │   │   ├── remote_backend.h / .cpp
│   │   │   └── sse_stream_reader.h / .cpp
│   │   └── local/
│   │       └── local_backend.h / .cpp   # (Phase 5, 可选)
│   └── agent/                           # Agent 层（未来，仅接口）
│       ├── i_agent_core.h
│       ├── i_tool.h
│       └── agent_events.h
├── tests/
│   ├── CMakeLists.txt
│   ├── test_line_editor.cpp
│   ├── test_chat_session.cpp
│   ├── test_command_router.cpp
│   └── test_sse_parser.cpp
└── third_party/
```

---

## 8. 第三方依赖

| 库 | 用途 | 集成方式 |
|----|------|---------|
| nlohmann/json | JSON 处理 | vcpkg，必需 |
| libcurl | HTTP 客户端 (RemoteBackend) | vcpkg，必需，C API 需轻量封装 |
| llama.cpp | 本地推理 (LocalBackend) | git submodule，可选（Phase 5） |

**关键决策**：
- **LineEditor**：自研，参考 llama.cpp readline_advanced（~400 行），无外部依赖，支持 UTF-8/CJK/历史/Tab 补全
- **实现起点**：Phase 1 先做 TUI 骨架 + 回显，Phase 2 再接 Backend

mydev 组件（Result, EventBus, TaskManager, SSEParser, ConfigManager）直接复制到 `src/core/`。

---

## 9. 实现阶段

### Phase 1: 基础骨架（TUI 能跑）

1. 创建 CMakeLists.txt + vcpkg.json + 目录结构
2. 复制 mydev 组件到 src/core/
3. 实现 IPlatform + platform_win32.cpp（主平台）
4. 实现 LineEditor（从 llama.cpp readline_advanced 移植，UTF-8/CJK/历史/Tab 补全）
5. 实现 Terminal（封装 IPlatform + LineEditor + 输出着色）
6. 定义 events.h（所有事件结构体）
7. 实现最小 main.cpp：Terminal 初始化 → 订阅 UserInputEvent → 回显

**验证**：workx 启动，接受输入，彩色回显

### Phase 2: Backend + Session（能聊天）✅ 已完成

1. ✅ 实现 chat_types.h（ChatMessage, CompletionRequest, StreamChunk, BackendConfig, ModelInfo）
2. ✅ 实现 IBackend + IStreamReader + ICompletionProvider 接口
3. ✅ 实现 RemoteBackend（libcurl HTTP + SSEParser 流式）
4. ✅ 实现 ChatSession（持有 ICompletionProvider，处理 UserInputEvent，后台 Task 调用 IBackend，发 StreamTokenEvent）
5. ✅ 实现 CommandRouter（解析 /exit, /clear, /regen, /help）
6. ✅ 组装 main.cpp：RemoteBackend → ChatSession → EventBus

**新增文件**：
- `src/backend/chat_types.h` — ChatMessage, CompletionRequest, StreamChunk, BackendConfig, ModelInfo
- `src/backend/i_stream_reader.h` — StreamState 枚举, IStreamReader 接口
- `src/backend/i_completion_provider.h` — ICompletionProvider 统一接口

**修改文件**：
- `src/backend/i_backend.h` — 继承 ICompletionProvider，添加 submit/get_model_info
- `src/backend/remote/remote_backend.h/.cpp` — 完整实现 libcurl HTTP + SSE
- `src/backend/remote/sse_stream_reader.h/.cpp` — 完整实现 SSE 流解析
- `src/backend/backend_factory.h/.cpp` — 使用 BackendConfig 参数
- `src/session/chat_session.h/.cpp` — 完整实现对话状态机
- `src/session/command_router.h/.cpp` — 完整实现斜杠命令路由
- `src/main.cpp` — 组装所有层，添加 --remote/--model/--api-key 参数
- `CMakeLists.txt` — 添加 _CRT_SECURE_NO_WARNINGS

**验证**：编译成功（含/不含 CURL），回显模式正常运行

### Phase 3: TUI 完善（好体验）✅ 已完成

1. ✅ 实现 ChatRenderer（订阅流式事件，Terminal 彩色输出，reasoning 灰色显示）
2. ✅ 实现 Spinner（加载动画，集成到 ChatRenderer：推理开始显示，首个 token 停止）
3. ✅ 实现 History（持久化命令历史，启动加载 ~/.WORKX_history，退出保存）
4. ✅ Tab 补全：命令补全 + 文件路径补全
5. ✅ 多行输入（反斜杠续行）
6. ✅ Ctrl+C 双击退出
7. ✅ 欢迎横幅

**关键改进**：
- Spinner 改为通过 `Terminal::write_safe/move_cursor_safe` 线程安全方法操作
- Terminal 添加 `m_output_mutex` 保护所有 IPlatform 输出操作
- ChatSession 推理开始时发布 `BackendStatusEvent::Connecting` 触发 Spinner
- History 类新增 `load/save` 方法，Terminal 初始化/退出时自动持久化
- LineEditor 新增 `load_history/get_history` 接口

**验证**：编译成功，回显模式正常，历史文件正确创建

### Phase 4: 配置 + 健壮性 ✅ 已完成

1. ✅ 集成 ConfigManager（CLI 参数 > 环境变量 > 配置文件 > 默认值）
2. ✅ CLI 参数解析：--remote, --model, --api-key, --simple-io, --no-color, --system-prompt, --config
3. ✅ Result 错误处理贯穿所有可失败操作
4. ✅ ChatSession 重试逻辑（指数退避，可配置 retry_count/retry_delay_ms）
5. ✅ 会话持久化（/save, /load 命令，JSON 格式）
6. ✅ 单元测试（Catch2，20 个测试全通过）

**关键改进**：
- main.cpp 用 ConfigManager 替换 CliArgs，配置优先级：CLI > 环境变量 > 配置文件 > 默认值
- 新增 `--config <path>` 参数加载自定义配置文件
- 新增 `--system-prompt` 参数
- 环境变量支持：WORKX_API_KEY, WORKX_BASE_URL, WORKX_MODEL, WORKX_TIMEOUT, WORKX_NO_COLOR
- ConfigManager JSON 持久化启用（load_from_file/save_to_file）
- ChatSession 在 StreamState::Error 时自动重试（可配置次数和延迟）
- RemoteBackend 报告 HTTP 错误状态码
- CommandRouter 新增 /save 和 /load 命令
- ChatSession.save_session/load_session 支持对话历史序列化
- Catch2 v3 测试框架集成，20 个测试覆盖 ConfigManager/CommandRouter/SSEParser/ChatSession

### Phase 5: Local Backend（可选）

1. 实现 LocalBackend（包装 llama.cpp server_context + server_queue）
2. BackendFactory 根据配置创建对应后端
3. 模型加载进度条（ModelLoadEvent）

### Phase 6: Agent Core（未来）

1. 实现 IAgentCore + ITool 接口
2. 实现 AgentOrchestrator（ReAct 循环）
3. 实现 ToolRegistry + 内置工具
4. ChatSession 切换到 IAgentCore 作为 ICompletionProvider
5. ChatRenderer 支持 Agent 事件渲染

---

## 10. 关键源文件参考

| 文件 | 作用 |
|------|------|
| `plan/llama-cli-tui-analysis.md` | llama.cpp TUI 参考分析，Terminal/LineEditor 实现依据 |
| `.claude/skills/mydev/components/eventbus/src/event_bus.h` | EventBus 实现，架构骨干 |
| `.claude/skills/mydev/components/sse/src/sse_parser.hpp` | SSEParser，RemoteBackend 核心 |
| `.claude/skills/mydev/components/task/src/task_manager.h` | TaskManager，后台推理任务 |
| `.claude/skills/mydev/components/config/src/config_manager.h` | ConfigManager，分层配置 |
| `.claude/skills/mydev/components/result/src/result.h` | Result<T,E>，错误处理基础 |
| `D:\develop\github\llama.cpp\common\console.h` | llama.cpp 控制台接口参考 |
| `D:\develop\github\llama.cpp\common\console.cpp` | 行编辑器实现参考 |
| `D:\develop\github\llama.cpp\tools\cli\cli.cpp` | 交互循环参考 |
| `D:\develop\github\llama.cpp\tools\server\server-queue.h` | 任务队列 + response reader 参考 |
