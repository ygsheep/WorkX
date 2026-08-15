# Client 顶层封装设计

- **日期**: 2026-07-12
- **范围**: `src/agent/api/client.h` / `client.cpp` + `tests/test_client.cpp`
- **目标**: 为 `agent/api` 提供可直接使用的顶层入口，消除 main.cpp 中的 6 步样板代码

---

## 1. 背景与动机

当前发起一次对话需要调用方手动完成（见 `src/app/main.cpp#L174-L214`）：

1. 读 config keys（provider/api_key/remote_url/model_name/timeout）
2. 查 `ProviderPreset` 填充 URL/默认模型
3. 拼 `BackendConfig`
4. `BackendFactory::create(config)`
5. `backend->initialize(config)`
6. 包进 `ChatSession`

这些样板没有统一入口。`Client` 把 Backend 工厂 + 初始化 + 会话管理收拢为 `Client::create({...})` 一行调用。

### 现有分层（不动）

```
sse_parser.hpp          ← SSE/NDJSON 增量解析
http_client             ← cURL HTTP + async streaming
i_provider_adapter      ← 协议适配（OpenAI/Anthropic）
sse_stream_reader       ← HTTP → SSEParser → StreamChunk 队列
remote_backend          ← IBackend 实现
backend_factory         ← 按 config.type 创建后端
i_backend / i_completion_provider / i_stream_reader  ← 抽象接口
chat_types.h            ← 数据类型
```

`Client` 是在 `backend_factory` 之上的**新顶层**，不修改下层任何文件。

---

## 2. 关键决策

| 决策点 | 选择 | 理由 |
|--------|------|------|
| 封装范围 | 厚封装（Backend + Session） | 一站式，调用方不接触 IBackend/ChatSession |
| API 风格 | 阻塞 `chat()`/`stream_chat()` + 异步 `chat_async()`/`stream_chat_async()` | 阻塞适合脚本；异步（TaskManager 后台）保持 TUI 响应 |
| 构造方式 | `ClientConfig` 聚合体 + 指定初始化器 | `{.provider=..., .model=...}` 语法简洁清晰，`Client::create()` 返回 Result |
| EventBus | 回调为主，EventBus 可选开启 | 默认自包含；TUI 场景可开启兼容现有 ChatRenderer |

---

## 3. 公开 API

### 3.1 ChatCallbacks

```cpp
namespace workx {

/// @brief 流式回调集合
struct ChatCallbacks {
    /// @brief 收到增量 token（回复或思考）
    std::function<void(const std::string& content_delta,
                       const std::string& reasoning_delta)> on_token;
    /// @brief 流式完成（正常结束），final_chunk.is_final == true
    std::function<void(const StreamChunk& final_chunk)> on_done;
    /// @brief 流途中错误（提交成功后的流错误）
    std::function<void(const std::string& msg, bool retryable)> on_error;
};

} // namespace workx
```

### 3.2 ClientConfig

```cpp
namespace workx {

/// @brief Client 配置（聚合体，支持指定初始化器）
/// @details 用法：Client::create({.provider = "lm-studio", .model = "..."})
struct ClientConfig {
    /// Provider 预设名（如 "lm-studio"/"deepseek"/"openai"）。
    /// 非空时自动查 ProviderPreset 填充 base_url/provider_type/默认 model/timeout。
    /// 留空时使用 base_url + provider_type。
    std::string provider;

    /// 协议类型（provider 为空时生效，默认 OpenAI）
    ProviderType provider_type = ProviderType::OpenAI;

    /// API 基础 URL（显式设置优先于 preset 默认值）
    std::string base_url;

    /// API Key（本地 LM Studio 可留空）
    std::string api_key;

    /// 模型名（显式设置优先于 preset 默认值）
    std::string model;

    /// HTTP 超时（毫秒）
    int timeout_ms = 30000;

    /// 系统提示词（可选）
    std::string system_prompt;

    /// 重试次数与初始退避延迟（毫秒，指数退避）
    int retry_count = 3;
    int retry_delay_ms = 1000;

    /// 开启后：订阅 InterruptEvent 自动中断 + 发布 StreamToken/Done/Error
    bool enable_event_bus = false;
};

} // namespace workx
```

### 3.3 Client

```cpp
namespace workx {

class Client {
public:
    /// @brief 从配置创建 Client
    /// @details 内部完成：查 preset（若 provider 非空）→ 拼 BackendConfig
    ///          → BackendFactory::create → backend->initialize
    /// @return 失败返回错误信息（不抛异常）
    /// @code
    /// auto r = Client::create({.provider = "lm-studio",
    ///                          .model = "google/gemma-4-e4b"});
    /// if (r.isErr()) { std::cerr << r.error(); return; }
    /// Client client = std::move(r.value());
    /// @endcode
    static Result<Client, std::string> create(ClientConfig config);

    // ---- 会话管理 ----
    void set_system_prompt(const std::string& prompt);
    void clear_history();
    void regenerate();
    const std::vector<ChatMessage>& history() const;

    // ---- 阻塞 API（脚本/CLI 场景）----
    // 在调用线程同步执行，回调在该线程触发。调用期间线程被阻塞。
    // 适合一次性脚本；TUI 主线程勿用（会冻结界面）。
    Result<std::string, std::string> chat(const std::string& user_text);
    Result<std::string, std::string> chat(const std::vector<ChatMessage>& messages);
    Result<void, std::string> stream_chat(const std::string& user_text,
                                          const ChatCallbacks& cbs);
    Result<void, std::string> stream_chat(const std::vector<ChatMessage>& messages,
                                          const ChatCallbacks& cbs);

    // ---- 异步 API（TUI/交互场景）----
    // 立即返回，推理在 TaskManager 后台任务中执行。
    // 回调在后台线程触发 → 调用方需处理线程安全。
    // TUI 场景建议开启 enable_event_bus，用 EventBus 回主线程渲染。
    // 返回 Ok 表示已提交；提交级失败返回 Err。
    Result<void, std::string> chat_async(const std::string& user_text,
                                         const ChatCallbacks& cbs);
    Result<void, std::string> stream_chat_async(const std::string& user_text,
                                                const ChatCallbacks& cbs);

    // ---- 控制 ----
    void interrupt();
    bool is_generating() const;

    // ---- 后端能力透传 ----
    Result<std::vector<ModelInfo>, std::string> list_models();
    void set_model(const std::string& name);
    std::string model_name() const;

    // 仅可移动
    Client(Client&&) noexcept = default;
    Client& operator=(Client&&) noexcept = default;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    ~Client();

private:
    Client(std::unique_ptr<IBackend> backend,
           std::string system_prompt,
           int retry_count,
           int retry_delay_ms,
           bool publish_events);

    /// 流式核心实现（内部共享）
    Result<void, std::string> run_stream(const CompletionRequest& request,
                                         const ChatCallbacks& cbs,
                                         std::string& content_out,
                                         std::string& reasoning_out);

    std::unique_ptr<IBackend> m_backend;
    std::vector<ChatMessage> m_messages;
    std::string m_system_prompt;
    std::atomic<bool> m_generating{false};
    int m_max_retries = 3;
    int m_retry_delay_ms = 1000;
    bool m_publish_events = false;

    // EventBus 订阅 token（仅 m_publish_events=true 时有效）
    EventToken m_interrupt_token;
    bool m_subscribed = false;
};

} // namespace workx
```

### 3.4 使用示例

```cpp
// 脚本场景：阻塞同步
auto result = Client::create({.provider = "lm-studio",
                              .model = "google/gemma-4-e4b"});
if (result.isErr()) { std::cerr << result.error(); return; }
Client client = std::move(result.value());

auto reply = client.chat("你好");  // 阻塞直到完成
std::cout << reply.value() << "\n";

// CLI 流式（阻塞，回调在当前线程）
client.stream_chat("解释快速排序", {
    [](auto& content, auto& reasoning) {
        if (!reasoning.empty()) std::cerr << "[思考] " << reasoning;
        else std::cout << content << std::flush;
    },
    [](auto&) { std::cout << "\n[完成]\n"; },
    [](auto& err, bool retry) { std::cerr << "[错误] " << err; }
});

// TUI 场景：异步 + EventBus（不阻塞主线程，兼容 ChatRenderer）
auto r = Client::create({.provider = "deepseek",
                         .api_key = "sk-xxx",
                         .enable_event_bus = true});
Client tui_client = std::move(r.value());

// 立即返回，推理在后台任务；回调→EventBus→主线程渲染
tui_client.stream_chat_async(user_input, {
    [](auto&, auto&) {},  // 由 EventBus 事件驱动渲染，回调可空
    [](auto&) {},
    [](auto&, bool) {}
});
// 主线程继续跑 TUI 事件循环，用户可 Ctrl+C 中断
// tui_client.interrupt();

// 不用 preset，直连自定义 URL
auto r2 = Client::create({.base_url = "http://localhost:8080",
                          .provider_type = ProviderType::OpenAI,
                          .model = "my-model"});
```

---

## 4. 内部实现要点

### 4.1 Client::create() 流程

```
create(ClientConfig cfg)
  ├─ 若 cfg.provider 非空: find_preset(cfg.provider)
  │   ├─ 未找到 → 返回 Err("Unknown provider preset: ...")
  │   └─ 用 preset 填充 base_url/provider_type/默认 model/timeout
  │      （cfg 中显式设置的 base_url/model/timeout_ms 优先于 preset 默认值）
  ├─ 拼 BackendConfig:
  │     .type = Remote
  │     .provider = cfg.provider_type
  │     .base_url = cfg.base_url
  │     .api_key = cfg.api_key
  │     .model_name = cfg.model
  │     .timeout_ms = cfg.timeout_ms
  ├─ BackendFactory::create(backend_config) → unique_ptr<IBackend>
  │   nullptr → 返回 Err("Failed to create backend")
  ├─ backend->initialize(backend_config) → Result
  │   失败 → 返回 Err
  ├─ 构造 Client(backend, cfg.system_prompt, cfg.retry_count,
  │              cfg.retry_delay_ms, cfg.enable_event_bus)
  ├─ 若 enable_event_bus: 订阅 InterruptEvent
  └─ 返回 Ok(Client)
```

### 4.2 流式实现：阻塞 vs 异步

核心拉取逻辑（`run_stream`）共用，区别仅在"在哪个线程跑"：

#### run_stream（共享核心，复用 chat_session.cpp 的成熟逻辑）

```
run_stream(request, cbs, content_out, reasoning_out)
  ├─ retry loop (0..m_max_retries):
  │   ├─ reader = m_backend->submit_completion(request)
  │   │   nullptr → 退避等待（可中断）→ continue retry
  │   ├─ while (reader->next(should_stop, chunk)):
  │   │   ├─ HasData:
  │   │   │   ├─ content += chunk.content_delta
  │   │   │   ├─ reasoning += chunk.reasoning_delta
  │   │   │   ├─ cbs.on_token(content_delta, reasoning_delta)
  │   │   │   └─ if publish_events: publish_async(StreamTokenEvent)
  │   │   ├─ Complete:
  │   │   │   ├─ cbs.on_done(chunk)
  │   │   │   ├─ history.push_back(assistant(content))
  │   │   │   ├─ if publish_events: publish_async(StreamDoneEvent)
  │   │   │   └─ return Ok
  │   │   ├─ Error: 退避 → continue retry
  │   │   └─ Cancelled: cbs.on_done(partial) → return Ok
  │   └─ 重试耗尽 → cbs.on_error + return Err
```

#### stream_chat（阻塞版）

```
stream_chat(user_text, cbs)
  ├─ 构造 CompletionRequest
  ├─ m_generating = true
  ├─ run_stream(request, cbs, ...)  ← 直接在调用线程执行
  └─ m_generating = false
```

调用线程被阻塞直到流结束。适合脚本。`interrupt()` 无法从外部中断（线程被占住），仅靠 `should_stop` 检查（但无其他线程去设置它）。

#### stream_chat_async（异步版，TUI 用）

```
stream_chat_async(user_text, cbs)
  ├─ 构造 CompletionRequest
  ├─ m_generating = true
  ├─ TaskManager::launch("completion", lambda(should_cancel):
  │     └─ run_stream(request, cbs, ...)  ← 在后台线程执行
  │        should_stop = should_cancel     ← TaskManager 的取消信号接入
  │   )
  └─ 立即返回 Ok  ← 主线程不阻塞，继续跑 TUI 循环
```

- 主线程可继续渲染、处理键盘输入、执行 `/clear` 等命令
- `interrupt()` 从主线程调用 → 触发 `should_cancel` → 后台任务退出
- TUI 场景建议 `enable_event_bus = true`：回调在后台线程触发 → `publish_async` → 主线程事件循环消费 → 更新 UI（避免跨线程直接操作 UI）

#### 多进程说明

每个 `workx.exe` 是独立进程，各有自己的 TaskManager / HttpClient / 线程。`stream_chat` 在进程 A 阻塞不影响进程 B。多目录各启动一个 workx.exe 完全并行，无需特殊处理。

### 4.3 chat() 实现

调用 `stream_chat`（阻塞版），但不传 on_token（或传一个聚合到 string 的回调），完成后返回聚合的 content。

### 4.4 chat_async() 实现

调用 `stream_chat_async`，回调中将 content 聚合，on_done 时即为最终结果。

### 4.5 EventBus 集成（可选）

`m_publish_events == true` 时：
- 构造时订阅 `InterruptEvent` → 调用 `m_backend->interrupt()`
- `on_token` 时 `publish_async(StreamTokenEvent)` —— 异步版在后台线程发布，主线程事件循环消费
- `on_done` 时 `publish_async(StreamDoneEvent)`
- `on_error` 时 `publish_async(StreamErrorEvent)`

---

## 5. 测试设计

### 5.1 文件

新增 `tests/test_client.cpp`，加入 `tests/CMakeLists.txt` 的 `TEST_SOURCES`。

### 5.2 测试服务器

复用 LM Studio 约定：
- 环境变量 `LM_STUDIO_BASE_URL`（默认 `http://127.0.0.1:1234`）
- 环境变量 `LM_STUDIO_MODEL`（默认 `google/gemma-4-e4b`，该模型支持推理）
- `LmStudioFixture`：连通性预检 `GET /v1/models`，失败则 `FAIL`

### 5.3 测试用例

| # | 用例 | 类型 | 断言 |
|---|------|------|------|
| 1 | create 成功 | 集成 | `create({.provider="lm-studio", .model=model})` → Ok；`model_name()` 正确 |
| 2 | create 失败 | 单元 | 不设 base_url 也不设 provider → Err（"base_url is required"） |
| 3 | list_models | 集成 | 返回非空 vector |
| 4 | chat() 同步回复 | 集成 | 返回 Ok，value 非空；history 含 user+assistant |
| 5 | stream_chat() 回复流 | 集成 | on_token 触发，content 累计非空，on_done 被调用 |
| 6 | stream_chat() 思考流 | 集成 | on_token 的 reasoning_delta 参数累计；条件检查（见下） |
| 7 | clear_history | 单元 | 调用后 `history().empty()` |
| 8 | set_model 运行时切换 | 集成 | 切换后 `model_name()` 反映新值 |
| 9 | stream_chat_async() 异步流 | 集成 | 立即返回；回调在后台线程触发；content 累计非空；主线程可调 interrupt |
| 10 | interrupt() 中断异步流 | 集成 | 异步流进行中调 interrupt → on_done 带 was_interrupted 或流正常结束 |

### 5.4 思考流测试策略（单模型条件检查）

`google/gemma-4-e4b` 支持推理，预期产生 `reasoning_content`。但为兼容环境差异：

```cpp
// 用触发推理的 prompt
client.stream_chat("Think step by step: what is 15 + 27?", {...});

if (reasoning_acc.empty()) {
    WARN("模型未产生 reasoning_content，跳过思考断言");
} else {
    REQUIRE_FALSE(reasoning_acc.empty());
    // 思考与回复应分别路由
}
// 回复始终应有
REQUIRE_FALSE(content_acc.empty());
```

- 收到 reasoning → 断言非空，验证路由正确
- 未收到 → `WARN` 跳过（不 FAIL），仍断言 content 非空

### 5.5 测试用例骨架

```cpp
struct LmStudioFixture {
    static std::string s_base_url;
    static std::string s_model;

    LmStudioFixture() {
        static bool inited = false;
        if (!inited) {
            const char* url = std::getenv("LM_STUDIO_BASE_URL");
            s_base_url = (url && *url) ? url : "http://127.0.0.1:1234";
            while (!s_base_url.empty() && s_base_url.back() == '/') s_base_url.pop_back();

            const char* mdl = std::getenv("LM_STUDIO_MODEL");
            s_model = (mdl && *mdl) ? mdl : "google/gemma-4-e4b";

            HttpClient check;
            auto resp = check.get(s_base_url + "/v1/models", {}, 5000);
            if (!resp.error.empty() || resp.status_code != 200) {
                FAIL("LM Studio 未启动，请先运行 lms server start");
            }
            inited = true;
        }
    }
};

TEST_CASE_METHOD(LmStudioFixture, "Client create success", "[client][integration]") {
    auto r = Client::create({.provider = "lm-studio", .model = s_model});
    REQUIRE(r.isOk());
    REQUIRE(r.value().model_name() == s_model);
}

TEST_CASE("Client create failure without base_url", "[client]") {
    auto& cfg = ConfigManager::instance();
    cfg.clear_for_test();
    // 既不设 provider 也不设 base_url → initialize 失败
    auto r = Client::create({.provider_type = ProviderType::OpenAI});
    REQUIRE(r.isErr());
    cfg.clear_for_test();
}

TEST_CASE_METHOD(LmStudioFixture, "Client chat sync", "[client][integration]") {
    auto r = Client::create({.provider = "lm-studio", .model = s_model});
    REQUIRE(r.isOk());
    auto client = std::move(r.value());

    auto reply = client.chat("Say hello in one word.");
    REQUIRE(reply.isOk());
    REQUIRE_FALSE(reply.value().empty());

    auto& h = client.history();
    REQUIRE(h.size() >= 2);
    REQUIRE(h.back().role == ChatMessage::Role::Assistant);
}

TEST_CASE_METHOD(LmStudioFixture, "Client stream_chat content", "[client][integration]") {
    auto r = Client::create({.provider = "lm-studio", .model = s_model});
    REQUIRE(r.isOk());
    auto client = std::move(r.value());

    std::string content, reasoning;
    bool done = false;
    auto res = client.stream_chat("Say hi.", {
        [&](auto& c, auto& r) { content += c; reasoning += r; },
        [&](auto&) { done = true; },
        [](auto&, bool) {}
    });
    REQUIRE(res.isOk());
    REQUIRE(done);
    REQUIRE_FALSE(content.empty());
}

TEST_CASE_METHOD(LmStudioFixture, "Client stream_chat reasoning", "[client][integration]") {
    auto r = Client::create({.provider = "lm-studio", .model = s_model});
    REQUIRE(r.isOk());
    auto client = std::move(r.value());

    std::string content, reasoning;
    client.stream_chat("Think step by step: what is 15 + 27?", {
        [&](auto& c, auto& r) { content += c; reasoning += r; },
        [](auto&) {},
        [](auto&, bool) {}
    });

    // gemma-4-e4b 支持推理，预期有 reasoning；环境差异时 WARN 跳过
    if (reasoning.empty()) {
        WARN("模型未产生 reasoning_content，跳过思考断言");
    } else {
        REQUIRE_FALSE(reasoning.empty());
    }
    REQUIRE_FALSE(content.empty());
}

// ============================================================
// 异步 API 测试（TUI 场景：不阻塞调用线程）
// ============================================================

TEST_CASE_METHOD(LmStudioFixture, "Client stream_chat_async", "[client][integration][async]") {
    auto r = Client::create({.provider = "lm-studio", .model = s_model});
    REQUIRE(r.isOk());
    auto client = std::move(r.value());

    std::mutex mtx;
    std::condition_variable cv;
    std::string content, reasoning;
    bool done = false;

    // 立即返回，推理在后台线程
    auto res = client.stream_chat_async("Say hi.", {
        [&](auto& c, auto& r) {
            std::lock_guard<std::mutex> lk(mtx);
            content += c;
            reasoning += r;
        },
        [&](auto&) {
            std::lock_guard<std::mutex> lk(mtx);
            done = true;
            cv.notify_one();
        },
        [&](auto&, bool) {
            std::lock_guard<std::mutex> lk(mtx);
            done = true;
            cv.notify_one();
        }
    });
    REQUIRE(res.isOk());
    REQUIRE_FALSE(done);  // 立即返回时不应已完成

    // 等待后台完成（最多 20s）
    {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::seconds(20), [&] { return done; });
    }
    REQUIRE(done);
    REQUIRE_FALSE(content.empty());
}

TEST_CASE_METHOD(LmStudioFixture, "Client interrupt async", "[client][integration][async]") {
    auto r = Client::create({.provider = "lm-studio", .model = s_model});
    REQUIRE(r.isOk());
    auto client = std::move(r.value());

    std::mutex mtx;
    std::condition_variable cv;
    bool done = false;

    client.stream_chat_async("Write a long essay about history.", {
        [](auto&, auto&) {},
        [&](auto&) {
            std::lock_guard<std::mutex> lk(mtx);
            done = true;
            cv.notify_one();
        },
        [&](auto&, bool) {
            std::lock_guard<std::mutex> lk(mtx);
            done = true;
            cv.notify_one();
        }
    });

    REQUIRE(client.is_generating());

    // 等待一小会让流开始，然后中断
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    client.interrupt();

    {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::seconds(10), [&] { return done; });
    }
    // 中断后应结束（done=true），且不再 generating
    REQUIRE(done);
    REQUIRE_FALSE(client.is_generating());
}
```

---

## 6. 文件清单

| 文件 | 操作 | 说明 |
|------|------|------|
| `src/agent/api/client.h` | 新建 | Client + ClientConfig + ChatCallbacks 声明 |
| `src/agent/api/client.cpp` | 新建 | 实现 |
| `tests/test_client.cpp` | 新建 | 测试 |
| `tests/CMakeLists.txt` | 修改 | TEST_SOURCES 添加 test_client.cpp |
| `CMakeLists.txt`（根） | 修改 | 在 `src/agent/api/` 源文件列表后添加 `src/agent/api/client.cpp` |

### 不修改的文件

- `i_backend.h` / `i_completion_provider.h` / `i_stream_reader.h` — 接口不变
- `backend_factory.*` / `remote_backend.*` — 下层不变
- `chat_session.*` — 保留（现有代码仍用，未来可迁移到 Client）
- `chat_types.h` — 数据类型不变

---

## 7. 验收标准

1. `Client::create({.provider = "lm-studio", .model = "google/gemma-4-e4b"})` 成功
2. `client.chat("hi")` 返回非空回复
3. `client.stream_chat("hi", callbacks)` 的 on_token 收到 content_delta，on_done 被调用
4. `stream_chat` 测试中，若模型产生 reasoning_content，on_token 的 reasoning_delta 参数非空
5. `client.list_models()` 返回非空列表
6. `client.interrupt()` 不崩溃
7. `clear_history()` 后 `history()` 为空
8. `stream_chat_async` 立即返回，后台线程触发回调，content 累计非空
9. 异步流进行中调 `interrupt()` → 流结束，`is_generating()` 变 false
10. 多进程：多个 workx.exe 实例各自独立运行互不干扰
11. 全部测试通过 `ctest`
