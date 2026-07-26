# WorkX 第三阶段：长期技术债重构方案

> **文档版本**: 1.0  
> **基于分析**: ARCH_ANALYSIS_REPORT.md (2026-07-27)  
> **前置条件**: Phase 1~2 已完成（EventBus/TaskManager/ReActLoop DI 化、线程安全修复）  
> **预估工期**: 4~6 周（可与业务需求并行，逐项推进）

---

## 目录

1. [总体策略](#一总体策略)
2. [债务 L：生命周期与裸指针](#二债务-l生命周期与裸指针)
3. [债务 H：HTTP 客户端演进](#三债务-hhttp-客户端演进)
4. [债务 C：配置系统完善](#四债务-c配置系统完善)
5. [债务 G：日志系统治理](#五债务-g日志系统治理)
6. [债务 T：残余线程安全问题](#六债务-t残余线程安全问题)
7. [债务 D：依赖注入深化](#七债务-d依赖注入深化)
8. [债务 E：错误处理统一（V2 前置）](#八债务-e错误处理统一v2-前置)
9. [实施路线图](#九实施路线图)
10. [验收总纲](#十验收总纲)

---

## 一、总体策略

### 1.1 原则

| 原则 | 说明 |
|------|------|
| **渐进式** | 每次只改一个债务项，不一次性全改，降低回归风险 |
| **测试先行** | 每个重构前补充/强化对应模块的单元测试，确保行为不变 |
| **向后兼容** | 公共 API 变更时保留旧接口做 `[[deprecated]]` 转发，至少一个版本周期 |
| **日志追踪** | 重构过程中补充关键路径 LOG，便于后续问题定位 |

### 1.2 技术债务总览

```
┌─────────────────────────────────────────────────────────────┐
│  优先级矩阵                                                   │
│                                                             │
│  高影响 ↑  │  L-1 裸指针      H-1 HTTP 连接池               │
│           │  T-4/T-5 原子化   G-2 Logger detach              │
│           ├─────────────────────────────────────────────────┤
│           │  C-2 配置 schema   D-2 DI 容器                   │
│           │  E-1 错误统一                                 │
│           ├─────────────────────────────────────────────────┤
│  低影响 ↓  │  C-4 环境变量文档  H-4 URL 解析增强              │
│           │  G-3 命名空间统一                               │
│           └─────────────────────────────────────────────────┘
│                低紧急 ←────────────────────────→ 高紧急      │
└─────────────────────────────────────────────────────────────┘
```

---

## 二、债务 L：生命周期与裸指针

### 2.1 问题诊断

当前代码中存在多处裸指针传递所有权或依赖关系，违反现代 C++ 所有权语义：

| 编号 | 位置 | 裸指针 | 问题 |
|------|------|--------|------|
| L-1 | `src/app/main.cpp:170` | `IBackend* g_backend` | 栈上 `unique_ptr<ChatSession>` 拥有的 `IBackend` 裸指针跨函数传递，Session 析构后 `g_backend` 悬空 |
| L-2 | `src/agent/core/react_loop.h:296` | `ICompletionProvider* m_provider` | 文档约束"非拥有"，但无编译期保证；移动后指针失效风险 |
| L-3 | `src/tui/render/chat_renderer.h:66` | `Terminal* m_terminal` | Renderer 生命周期长于 Terminal（Terminal 先 restore 再析构 Renderer），但两者栈顺序不保证 |
| L-5 | `src/agent/api/remote/sse_stream_reader.h` | `StreamSession::m_multi` | CURLM* 在 HttpClient::Impl 析构时 cleanup，StreamSession 可能仍持有引用 |
| L-6 | `src/agent/api/client.h:196` | `ITaskManager* m_task_manager` | Client 可移动，用指针避免引用无法重新绑定，但移动后原对象指针悬空 |

### 2.2 影响分析

- **L-1**: `main.cpp` 中 `g_backend` 被 `ModelSelector` 回调、`SystemCommandContext` 多处捕获。若 `session` 被 `unique_ptr` reset（如异常路径），`g_backend` 立即悬空，后续 `select_model_interactive` 等调用 UB。
- **L-2**: `ReActLoop` 可移动（`ReActLoop&& = default`），移动后 `m_provider` 指向原地址，但原对象已析构或被覆盖。
- **L-3**: `Terminal::~Terminal()` 调用 `restore()`，`ChatRenderer::~ChatRenderer()` 在 `Terminal` 之后析构（栈顺序），但 `ChatRenderer::stop()` 在 `main()` 中显式调用，此时 `m_terminal` 仍有效。风险较低，但代码依赖调用顺序。
- **L-5**: `HttpClient::Impl::~Impl()` 先 `curl_multi_cleanup(multi)`，但 `StreamSession` 析构需要 `m_multi` 做 `curl_multi_remove_handle`。当前 `shutdown()` 先清空 `sessions_by_handle`（shared_ptr 释放导致 StreamSession 析构），再 `curl_multi_cleanup`，顺序正确但隐式依赖。
- **L-6**: `Client` 移动后，原对象的 `m_task_manager` 被置 `nullptr`，新对象指向同一个 `ITaskManager`。若原对象析构后新对象仍使用，安全（因为 `m_task_manager` 不是拥有关系）。问题在于 `Client` 的默认构造（无默认构造），移动语义本身正确。

### 2.3 重构目标

将所有"非拥有但需保证生命周期"的裸指针替换为编译期安全的引用类型。

### 2.4 实施方案

#### L-1: `g_backend` → `std::reference_wrapper<IBackend>` 或彻底消除

**当前代码** (`main.cpp:170`):
```cpp
IBackend* g_backend = nullptr;  // raw ptr, owned by ChatSession
```

**问题**: `g_backend` 被多处 lambda 捕获（`sys_ctx.on_model_select`、`set_completion_callback`、`terminal.set_ctrl_o_callback` 等），若 `session` 被 reset 则悬空。

**方案 A（推荐）**: 消除 `g_backend`，所有需要 backend 的地方通过 `session->backend()` 获取（需新增 `ChatSession::backend()` 访问器）。`session` 的 `unique_ptr` 在 `main()` 中存活到函数末尾，通过 `session.get()` 获取 backend 始终有效。

```cpp
// main.cpp 修改后
// 删除: IBackend* g_backend = nullptr;

// 所有 g_backend 替换为 session->backend()
sys_ctx.on_model_select = [&terminal, &screen, &session, &cfg, &renderer, &preset]() {
    auto* backend = session->backend();  // 通过 ChatSession 获取
    if (!backend) { /* ... */ return; }
    ModelSelection sel = select_model_interactive(&terminal, &screen, backend, ...);
    // ...
};
```

**方案 B**: 若无法消除，用 `std::optional<std::reference_wrapper<IBackend>>`：
```cpp
std::optional<std::reference_wrapper<IBackend>> g_backend;
// 初始化
g_backend = *backend;
// 使用
if (g_backend) g_backend->get().set_model_name(...);
```

**新增接口** (`chat_session.h`):
```cpp
class ChatSession {
public:
    /// @brief 获取底层 backend（供 UI 层获取模型列表等）
    IBackend* backend() const { return dynamic_cast<IBackend*>(m_provider.get()); }
};
```

---

#### L-2: `ReActLoop::m_provider` → `not_null<ICompletionProvider*>` 或引用

**当前代码**:
```cpp
ICompletionProvider* m_provider;  // 非拥有
```

**问题**: `ReActLoop` 支持移动（`ReActLoop(ReActLoop&&) = default`），移动后 `m_provider` 被默认移动（按值拷贝指针），指向原地址。若原对象析构，新对象的 `m_provider` 悬空。

**方案**: 禁用 `ReActLoop` 的移动语义（因为 `m_provider` 是指向外部对象的引用），或改用 `std::reference_wrapper<ICompletionProvider>`：

```cpp
class ReActLoop {
public:
    ReActLoop(const ReActLoop&) = delete;
    ReActLoop& operator=(const ReActLoop&) = delete;
    // 删除移动：引用不可移动
    ReActLoop(ReActLoop&&) = delete;
    ReActLoop& operator=(ReActLoop&&) = delete;
    
private:
    std::reference_wrapper<ICompletionProvider> m_provider;
};
```

**影响**: `ChatSession::run_completion()` 中 `ReActLoop loop(...)` 是栈局部变量，不移动，无影响。

---

#### L-3: `ChatRenderer::m_terminal` → 弱引用或生命周期契约

**当前代码**:
```cpp
Terminal* m_terminal;
```

**分析**: `main()` 中显式调用 `renderer.stop()` 后 `terminal.restore()`，顺序正确。`ChatRenderer` 的 `start()/stop()` 已管理事件订阅生命周期。

**方案**: 添加 `NOT_NULL` 断言 + 文档化生命周期契约，不改动指针类型（因为 `Terminal` 和 `ChatRenderer` 是同层对象，由 `main()` 统一控制）：

```cpp
class ChatRenderer {
    ChatRenderer(Terminal* terminal) : m_terminal(terminal) {
        assert(terminal != nullptr && "Terminal must outlive ChatRenderer");
    }
};
```

---

#### L-5: `StreamSession::m_multi` → 安全句柄包装

**当前代码**:
```cpp
CURLM* m_multi = nullptr;  // 由 HttpClient::Impl 拥有
```

**问题**: `StreamSession` 析构时需要 `m_multi` 做 `curl_multi_remove_handle`，但 `m_multi` 在 `HttpClient::Impl::shutdown()` 中被 `curl_multi_cleanup`。

**当前保护**:
```cpp
~StreamSession() {
    if (m_curl && m_added_to_multi.load() && m_multi) {
        curl_multi_remove_handle(m_multi, m_curl);
    }
}
```

**分析**: `HttpClient::Impl::shutdown()` 先清空 `sessions_by_handle`（触发 `StreamSession` 析构），再 `curl_multi_cleanup`，顺序安全。但依赖 `shared_ptr` 的释放顺序。

**方案**: 提取 `CurlMultiHandle` RAII 类，确保 cleanup 在所有 session 移除之后：

```cpp
class CurlMultiHandle {
public:
    explicit CurlMultiHandle() : m_multi(curl_multi_init()) {}
    ~CurlMultiHandle() { if (m_multi) curl_multi_cleanup(m_multi); }
    
    void add(CURL* easy) { curl_multi_add_handle(m_multi, easy); }
    void remove(CURL* easy) { curl_multi_remove_handle(m_multi, easy); }
    CURLM* get() const { return m_multi; }
    
private:
    CURLM* m_multi;
};
```

---

#### L-6: `Client::m_task_manager` → 无需改动

**分析**: `Client` 的移动构造函数正确地将 `m_task_manager` 从原对象复制到新对象，并将原对象置 `nullptr`。由于 `ITaskManager` 不是 `Client` 拥有的（是外部单例或注入对象），移动后两个对象中只有一个有效指针，符合预期。`nullptr` 检查已在所有使用处存在（如 `chat_async` 中 `m_task_manager->launch(...)`，若 `m_task_manager` 为 `nullptr` 则崩溃，但构造时保证非空）。

**结论**: 维持现状，在 `Client` 析构时添加 `assert(m_task_manager != nullptr)` 即可。

### 2.5 验收标准

- [ ] `grep -r "IBackend\*" src/app/main.cpp` 无匹配（或仅剩 `dynamic_cast`）
- [ ] `ReActLoop` 移动构造函数被删除，编译器报错若试图移动
- [ ] `ChatSession::backend()` 访问器可用，单元测试验证
- [ ] 所有裸指针使用处添加 `assert(ptr != nullptr)` 或改用引用类型

---

## 三、债务 H：HTTP 客户端演进

### 3.1 问题诊断

| 编号 | 问题 | 当前状态 | 影响 |
|------|------|----------|------|
| H-1 | 无连接池 | 每次 `async_post_stream` 新建 `CURL*`（libcurl 内部复用连接，但无显式连接池管理） | 高频请求时 TCP 握手 overhead |
| H-2 | 总时长超时缺失 | 流式传输用 `LOW_SPEED_TIME`（空闲超时），无整体请求时长上限 | 长响应（reasoning model 60s+）正常，但异常慢响应无总体上限 |
| H-3 | 重试逻辑分散 | `Client::run_stream` 内含重试，`RemoteBackend` 无重试，`HttpClient` 无重试 | 各层重复实现或缺失 |
| H-4 | URL 解析冗余 | `HttpClient::parse_url` 手写 fallback + libcurl CURLU API | 维护两套解析逻辑 |

### 3.2 重构目标

将 `HttpClient` 从"裸 curl 封装"升级为"生产级 HTTP 客户端"：连接池、统一超时、分层重试。

### 3.3 实施方案

#### H-1: 连接池（轻量版）

libcurl 本身有连接缓存（`CURLM` 内部的 connection cache），但当前每个 `HttpClient` 实例独立持有一个 `CURLM*`，多个 `HttpClient` 实例间不共享连接。

**方案**: 在 `HttpClient::Impl` 中使用 `curl_multi` 的连接复用能力 + `CURLSH` 共享接口（若需要跨 `HttpClient` 实例共享）：

```cpp
struct HttpClient::Impl {
    // 当前：每个 Impl 一个 CURLM
    CURLM* multi;
    
    // 改进：使用 CURLSH 共享连接缓存（跨 HttpClient 实例）
    static CURLSH* shared_cache() {
        static CURLSH* sh = curl_share_init();
        static std::once_flag init;
        std::call_once(init, [&]() {
            curl_share_setopt(sh, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
        });
        return sh;
    }
};
```

**更简单的方案**: 单例 `CURLM`（全局唯一），所有 `HttpClient` 实例共享：

```cpp
class GlobalCurlMulti {
public:
    static CURLM* instance() {
        static GlobalCurlMulti inst;
        return inst.m_multi;
    }
    
private:
    GlobalCurlMulti() { m_multi = curl_multi_init(); }
    ~GlobalCurlMulti() { curl_multi_cleanup(m_multi); }
    CURLM* m_multi;
};
```

**注意**: 全局 `CURLM` 意味着所有流共享同一个 poll 线程，需确保线程安全（libcurl 的 `CURLM` 不是线程安全的，所有操作需在同一线程或加锁）。当前设计已满足（所有操作在 `Impl::poll_thread` 中进行）。

---

#### H-2: 总时长超时（Timer-based）

**当前**: 流式传输用 `LOW_SPEED_TIME`（空闲超时），无总时长限制。

**问题场景**: 服务器持续以极低速率发送数据（如 1 byte/s），`LOW_SPEED_TIME` 因"有数据传输"而不触发，但请求实际已异常缓慢。

**方案**: 在 `StreamSession` 中增加总时长计时器：

```cpp
class StreamSession {
public:
    StreamSession(..., int total_timeout_ms = 120000)  // 默认 2 分钟总超时
        : m_total_timeout_ms(total_timeout_ms)
        , m_start_time(std::chrono::steady_clock::now())
    {}
    
    bool is_total_timeout() const {
        auto elapsed = std::chrono::steady_clock::now() - m_start_time;
        return std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > m_total_timeout_ms;
    }
    
private:
    int m_total_timeout_ms;
    std::chrono::steady_clock::time_point m_start_time;
};
```

在 `Impl::poll_loop` 中定期检查：
```cpp
void poll_loop() {
    while (running.load()) {
        // ... 现有 poll 逻辑 ...
        
        // 新增：总时长超时检查
        {
            std::lock_guard<std::mutex> lock(sessions_mutex);
            for (auto& [handle, session] : sessions_by_handle) {
                if (session->is_total_timeout()) {
                    session->cancel();
                    session->finish_with_error("Total request timeout exceeded");
                }
            }
        }
        
        curl_multi_wait(multi, nullptr, 0, 100, nullptr);
    }
}
```

---

#### H-3: 重试逻辑分层

**当前状态**:
- `Client::run_stream`: 指数退避重试（submit 失败 + 流错误）
- `ChatSession::run_completion`: 指数退避重试（仅流错误）
- `HttpClient`: 无重试
- `RemoteBackend`: 无重试

**问题**: 重试逻辑散落在 `Client` 和 `ChatSession` 两层，且策略不一致。

**方案**: 提取 `HttpRetryPolicy` 策略类，统一配置：

```cpp
// agent/api/retry_policy.h [新增]
#pragma once
#include <functional>
#include <chrono>

namespace agent {

struct HttpRetryPolicy {
    int max_retries = 3;
    int base_delay_ms = 1000;
    int max_delay_ms = 60000;      // 60 秒上限
    int total_timeout_ms = 120000; // 2 分钟总超时
    
    // 可重试错误判断（5xx、连接超时、DNS 失败等）
    bool is_retryable(unsigned int http_status, const std::string& curl_error) const;
    
    // 计算第 N 次重试的延迟
    std::chrono::milliseconds delay(int attempt) const;
};

} // namespace agent
```

`RemoteBackend` 构造时注入 `HttpRetryPolicy`，`Client` 和 `ChatSession` 不再自己实现重试，而是委托给 `RemoteBackend`。

**重构后调用链**:
```
Client::chat()
  → RemoteBackend::submit_completion()  [内部含重试]
    → HttpClient::async_post_stream()   [无重试，仅单次请求]
```

---

#### H-4: URL 解析统一

**当前**: `parse_url` 先尝试 `CURLU` API，失败后用自定义 fallback。两套逻辑可能产生不一致结果。

**方案**: 完全依赖 `CURLU`（libcurl 7.62.0+ 提供），删除自定义 fallback：

```cpp
ParsedUrl HttpClient::parse_url(const std::string& url) {
    ParsedUrl result;
    CURLU* hurl = curl_url();
    if (!hurl || curl_url_set(hurl, CURLUPART_URL, url.c_str(), 0) != CURLUE_OK) {
        if (hurl) curl_url_cleanup(hurl);
        throw std::invalid_argument("Invalid URL: " + url);
    }
    // ... 提取各 part ...
    curl_url_cleanup(hurl);
    return result;
}
```

**兼容性**: `CURLU` 需要 libcurl 7.62.0+（2018 年发布）。项目使用 vcpkg 管理依赖，可确保版本。若需兼容旧版，在 CMake 中检测 `CURLU` 可用性，不可用时降级为 `std::regex` 解析（但仍只保留一套逻辑）。

### 3.4 验收标准

- [ ] `HttpClient` 支持 `CURLSH` 连接共享（或文档说明复用机制）
- [ ] `StreamSession` 总时长超时可配置，默认 120 秒
- [ ] `RemoteBackend` 提供 `set_retry_policy()`，`Client`/`ChatSession` 重试逻辑委托
- [ ] URL 解析仅一套逻辑，单元测试覆盖各种格式

---

## 四、债务 C：配置系统完善

### 4.1 问题诊断

| 编号 | 问题 | 当前状态 |
|------|------|----------|
| C-2 | 配置无 schema | `ConfigMeta` 提供默认值和验证回调，但无结构化 schema（类型、范围、枚举值） |
| C-3 | `ConfigScope` 未落地 | `ConfigScope` 类存在但核心方法仍直接调用 `ConfigManager::instance()`，未真正使用 |
| C-4 | 环境变量无文档 | `load_from_env()` 读取哪些环境变量？无集中文档 |

### 4.2 实施方案

#### C-2: 配置 Schema

当前 `ConfigMeta`:
```cpp
struct ConfigMeta {
    std::string description;
    ConfigValue default_value;
    bool is_required = false;
    ValidateCallback validate_callback;
    ChangeCallback change_callback;
};
```

**增强为 Schema**:

```cpp
struct ConfigSchema {
    std::string key;
    std::string description;
    ConfigValue default_value;
    bool is_required = false;
    
    // 类型约束
    enum Type { Bool, Int, Double, String, Enum } type;
    
    // 范围约束（仅 Int/Double）
    std::optional<std::pair<int64_t, int64_t>> int_range;
    std::optional<std::pair<double, double>> double_range;
    
    // 枚举值（仅 Enum）
    std::vector<std::string> enum_values;
    
    // 验证函数
    std::function<Result<void, std::string>(const ConfigValue&)> validate;
};
```

**注册示例**:
```cpp
void register_config_defaults() {
    ConfigManager::instance().register_schema({
        .key = "backend.timeout_ms",
        .description = "HTTP request timeout in milliseconds",
        .default_value = 30000,
        .type = ConfigSchema::Int,
        .int_range = {1000, 300000},  // 1s ~ 5min
    });
    
    ConfigManager::instance().register_schema({
        .key = "backend.provider",
        .description = "LLM provider preset",
        .default_value = std::string("openai"),
        .type = ConfigSchema::Enum,
        .enum_values = {"openai", "anthropic", "deepseek", "groq", "lm-studio"},
    });
}
```

**自动验证**: `ConfigManager::set_value()` 时自动查询 schema 并校验：
```cpp
Result<void, std::string> ConfigManager::set_value(const std::string& key, ConfigValue value) {
    auto schema_it = m_schemas.find(key);
    if (schema_it != m_schemas.end()) {
        auto validation = schema_it->second.validate(value);
        if (validation.isErr()) return validation;
    }
    // ... 原有逻辑 ...
}
```

---

#### C-3: `ConfigScope` 落地

**当前问题**: `ConfigScope` 内部直接调用 `ConfigManager::instance()`，未实现真正的作用域隔离：

```cpp
class ConfigScope {
    template<typename T>
    Result<void, std::string> set(const std::string& key, T value) {
        return ConfigManager::instance().set(make_key(key), std::move(value));  // 仍用单例
    }
};
```

**重构**: 让 `ConfigScope` 持有 `IConfigManager&` 引用，支持注入：

```cpp
class ConfigScope {
public:
    explicit ConfigScope(const std::string& prefix, IConfigManager& cm = ConfigManager::instance())
        : m_prefix(prefix), m_config(cm) {}
    
    template<typename T>
    Result<void, std::string> set(const std::string& key, T value) {
        return m_config.get().set(make_key(key), std::move(value));
    }
    
private:
    std::string m_prefix;
    std::reference_wrapper<IConfigManager> m_config;
};
```

---

#### C-4: 环境变量文档与自动绑定

**当前**: `load_from_env()` 读取哪些变量分散在代码中，无集中文档。

**方案**: 在 `ConfigSchema` 中增加环境变量映射：

```cpp
struct ConfigSchema {
    // ... 原有字段 ...
    std::string env_var;  // 对应的环境变量名，如 "WORKX_API_KEY"
};
```

自动加载：
```cpp
void ConfigManager::load_from_env() {
    for (const auto& [key, schema] : m_schemas) {
        if (!schema.env_var.empty()) {
            const char* val = std::getenv(schema.env_var.c_str());
            if (val) {
                // 按 schema.type 解析 val
                set_value(key, parse_env_value(val, schema.type));
            }
        }
    }
}
```

**生成文档**: 添加脚本/工具从 schema 生成 Markdown 配置文档：
```bash
# 生成 docs/configuration.md
workx --dump-config-schema > docs/configuration.md
```

### 4.4 验收标准

- [ ] `ConfigManager::register_schema()` 可用，非法值设置返回错误
- [ ] `ConfigScope` 可通过构造注入 `MockConfigManager`
- [ ] 环境变量自动绑定，schema 中 `env_var` 非空时生效
- [ ] `workx --dump-config-schema` 输出完整配置文档

---

## 五、债务 G：日志系统治理

### 5.1 问题诊断

| 编号 | 问题 | 当前代码 | 影响 |
|------|------|----------|------|
| G-2 | `Logger` 析构 detach 写线程 | `logger.h:141` `m_writer_thread.detach()` | 进程退出时写线程可能仍在运行，日志丢失或崩溃 |
| G-3 | 命名空间不一致 | `Agent::Logger` vs 宏用 `Agent::Logger::get_instance()` vs 项目用 `agent::` | 混淆 |
| G-4 | `shared_ptr<Logger>` 单例 | `std::shared_ptr<Logger> get_instance()` | 无意义共享所有权，应为 `Logger&` 或 `Logger*` |

### 5.2 实施方案

#### G-2: 析构安全

**当前**:
```cpp
~Logger() {
    if (m_writer_thread.joinable()) {
        m_writer_running.store(false, std::memory_order_relaxed);
        m_queue_cv.notify_all();
        m_writer_thread.detach();  // ⚠️ 危险：线程可能仍在写文件
    }
    // 关闭文件流...
}
```

**问题**: `detach()` 后写线程可能继续访问 `m_file_stream`（已在主线程中关闭），导致 use-after-free。

**方案**: 改为 `join()`，确保所有队列数据写入后再关闭文件：

```cpp
~Logger() {
    if (m_writer_thread.joinable()) {
        m_writer_running.store(false, std::memory_order_release);
        m_queue_cv.notify_all();
        m_writer_thread.join();  // ✅ 等待写线程排空队列
    }
    // 此时队列已空，安全关闭文件
    {
        std::lock_guard<std::mutex> file_lock(m_file_mutex);
        if (m_file_stream.is_open()) {
            m_file_stream.flush();
            m_file_stream.close();
        }
    }
}
```

**风险**: `join()` 可能阻塞（若写线程卡在文件 I/O）。缓解：写线程使用有界队列，超限丢弃旧日志。

---

#### G-3: 命名空间统一

**当前混乱**:
```cpp
namespace Agent { class Logger; }          // 大写 A
namespace agent { /* 项目所有代码 */ }     // 小写 a
#define LOG_INFO(...) Agent::Logger::...   // 宏用大写
```

**方案**: 将 `Agent::Logger` 迁移到 `agent::log::Logger`（小写命名空间），保留旧命名空间做 `namespace Agent = agent::log;` 兼容：

```cpp
namespace agent::log {
    class Logger { /* ... */ };
}

namespace Agent = agent::log;  // 向后兼容
```

---

#### G-4: 单例返回类型

**当前**:
```cpp
static std::shared_ptr<Logger> get_instance() noexcept;
```

**问题**: `shared_ptr` 暗示共享所有权，但 `Logger` 是进程级单例，不应被共享拥有。

**方案**: 返回裸指针或引用：

```cpp
static Logger& get_instance() noexcept {
    static Logger instance(Token{});
    return instance;
}
```

**宏同步更新**:
```cpp
#define LOG_INFO(fmt, ...) agent::log::Logger::get_instance().info(std::format(fmt __VA_OPT__(,) __VA_ARGS__))
```

### 5.3 验收标准

- [ ] `Logger` 析构使用 `join()`，Valgrind/ASAN 无数据竞争报告
- [ ] 所有代码使用 `agent::log::Logger` 命名空间（旧 `Agent::` 做 alias）
- [ ] `get_instance()` 返回 `Logger&`，无 `shared_ptr`

---

## 六、债务 T：残余线程安全问题

### 6.1 问题诊断

`ARCH_REFACTOR_PLAN.md` Phase 3.5 声称"ChatRenderer 跨线程字段原子化已完成"，但实际代码中仍有非原子字段：

| 字段 | 类型 | 访问线程 | 风险 |
|------|------|----------|------|
| `m_reasoning_buffer` | `std::string` | 主循环写，Ctrl+O 回调读 | 非原子，但两者在同一线程（主循环），实际安全。注释说明"仅在 main loop 访问" |
| `m_thinking_start_time` | `time_point` | Spinner 线程写，主循环读 | 64-bit time_point 在 32-bit 平台可能非原子读取，数据撕裂 |
| `m_pending_tool_calls` | `unordered_map` | 事件回调（主循环）写/读 | `unordered_map` 非线程安全，但仅在主循环单线程访问，实际安全 |

**真正的问题**:
1. `Terminal` 中 `m_running`、`m_initialized` 为普通 `bool`（非原子），但 `m_event_pump_thread` 读 `m_event_pump_running`（原子），`shutdown()` 写 `m_running`（主线程）与 `run()` 读（主线程），实际同线程。
2. `StreamingBuffer::m_buffer` 受 `m_mutex` 保护，安全。
3. `DisplayBuffer` 未读取，但 `Terminal::write()` 在 `m_output_mutex` 保护下访问，安全。

**结论**: 经代码审查，`ChatRenderer` 的跨线程字段**实际已安全**（原子字段 + 单线程访问的 map/string）。`m_thinking_start_time` 在 64-bit 平台天然原子，风险极低。

**需修复**: 将 `m_thinking_start_time` 标记为原子或添加文档说明平台假设。

```cpp
// 方案：添加 static_assert 确保 time_point 是原子可读的
static_assert(sizeof(std::chrono::steady_clock::time_point) <= sizeof(uint64_t),
              "time_point must be trivially copyable for atomic-like reads");
```

### 6.2 真正需要修复的 T 债务

#### T-4: `Terminal::m_running` / `m_initialized` 原子化

虽然当前所有访问在同一线程，但为防御性编程，改为原子：

```cpp
std::atomic<bool> m_running{false};
std::atomic<bool> m_initialized{false};
```

#### T-5: `StreamingBuffer` 停止时 flush 竞争

```cpp
void StreamingBuffer::stop() {
    m_running.store(false, std::memory_order_release);
    m_cv.notify_all();
    if (m_thread.joinable()) m_thread.join();
    flush_now();  // 确保剩余内容写入
}
```

当前实现安全（`m_running` 原子，`m_mutex` 保护 `m_buffer`）。

#### T-6: `Task` 中 `m_start_time` 非原子

`m_start_time` 在 `execute()`（worker 线程）写，`markCompleted()`/`markFailed()`（同 worker 线程）读，单线程安全。`TaskManager::getRunningTasks()` 不访问 `m_start_time`。

**结论**: T 债务**实际风险较低**，主要工作是添加文档注释和 static_assert，无需大规模重构。

### 6.3 验收标准

- [ ] 所有跨线程 bool/flag 为 `std::atomic<bool>`
- [ ] `time_point` 平台假设有 static_assert 或文档说明
- [ ] ThreadSanitizer 运行无数据竞争报告（若可用）

---

## 七、债务 D：依赖注入深化

### 7.1 当前状态

Phase 2/4 已完成基础 DI：
- `IEventBus` → `EventBus`
- `ITaskManager` → `TaskManager`
- `IConfigManager` → `ConfigManager`

### 7.2 剩余问题

| 编号 | 问题 | 说明 |
|------|------|------|
| D-2 | 无 DI 容器 | `main.cpp` 中手动组装 10+ 个对象，依赖关系复杂时难以维护 |
| D-3 | `IBackend` 接口过重 | `IBackend` 继承 `ICompletionProvider`，但 `ChatSession` 只需 `ICompletionProvider`；`list_models()` 等能力被 `Client` 直接调用，绕过了 `ChatSession` |

### 7.3 实施方案

#### D-2: 轻量 DI 容器（可选）

**评估**: 项目规模不大（~200 个源文件），手动 DI 可维护。引入 DI 容器（如 Boost.DI、Hypodermic）会增加编译依赖和学习成本。

**轻量方案**: 用工厂函数 + 参数结构体替代：

```cpp
// app/di/factory.h [新增]
struct AppComponents {
    std::reference_wrapper<IEventBus> event_bus;
    std::reference_wrapper<ITaskManager> task_manager;
    std::reference_wrapper<IConfigManager> config_manager;
    std::unique_ptr<IBackend> backend;
    std::unique_ptr<ChatSession> session;
    std::unique_ptr<Terminal> terminal;
    // ...
};

AppComponents build_app(int argc, char* argv[]);
```

将 `main.cpp` 中 150+ 行的组装逻辑提取到 `build_app()`，使 `main()` 仅为：

```cpp
int main(int argc, char* argv[]) {
    try {
        auto app = build_app(argc, argv);
        app.terminal.get().run();
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
```

---

#### D-3: `IBackend` 接口拆分

**当前**: `IBackend : public ICompletionProvider`，接口包含 `list_models()`、`set_model_name()` 等管理能力。

**问题**: `ChatSession` 只关心 `ICompletionProvider`（提交推理、中断），但持有 `IBackend*` 可以调用不相关的管理接口。`Client` 直接调用 `IBackend` 的管理接口，绕过了 `ChatSession` 的会话管理。

**方案**: 拆分接口为最小契约：

```cpp
// ICompletionProvider — 推理能力（已有）
class ICompletionProvider {
    virtual std::shared_ptr<IStreamReader> submit_completion(const CompletionRequest&) = 0;
    virtual void interrupt() = 0;
    virtual bool is_generating() const = 0;
};

// IBackendAdmin — 后端管理能力（新增）
class IBackendAdmin {
    virtual std::string name() const = 0;
    virtual Result<std::vector<ModelInfo>, std::string> list_models() = 0;
    virtual void set_model_name(const std::string&) = 0;
    virtual ModelInfo get_model_info() const = 0;
};

// IBackend — 组合两者
class IBackend : public ICompletionProvider, public IBackendAdmin {
    virtual Result<void, std::string> initialize(const BackendConfig&) = 0;
    virtual void shutdown() = 0;
    virtual bool is_ready() const = 0;
};
```

`ChatSession` 只依赖 `ICompletionProvider`（已是当前设计，但构造参数为 `unique_ptr<ICompletionProvider>`，实际上传入的是 `IBackend` 的 `unique_ptr`）。

### 7.4 验收标准

- [ ] `main.cpp` 组装逻辑 < 20 行（委托给 `build_app()`）
- [ ] `IBackendAdmin` 接口独立存在，`Client` 通过 `IBackendAdmin*` 调用管理接口

---

## 八、债务 E：错误处理统一（V2 前置）

### 8.1 当前问题

4 种错误风格并存，详见 ARCH_ANALYSIS_REPORT.md §3.8。

### 8.2 统一方向

**不在本方案实施**，独立立项 `ARCH_REFACTOR_PLAN_V2.md`。本方案仅做前置准备：

1. **冻结新增错误风格**: 新代码统一使用 `Result<T, Error>`，禁止新增异常抛出点（除构造函数失败外）
2. **Error 类型设计**:
```cpp
struct Error {
    enum Code {
        OK = 0,
        NetworkTimeout,
        NetworkDisconnected,
        HTTPError,           // 4xx/5xx
        InvalidInput,
        PermissionDenied,
        ResourceNotFound,
        InternalError,
        Cancelled,
    } code;
    std::string message;
    std::string context;   // 额外上下文（如请求 ID、URL）
    
    bool is_retryable() const;
};

template<typename T>
using ResultV2 = std::variant<T, Error>;
```
3. **过渡期兼容**: `Result<T, std::string>` 逐步迁移到 `ResultV2<T>`，旧接口标记 `[[deprecated("Use ResultV2")]]`

---

## 九、实施路线图

### 阶段划分

```
Week 1~2: L 债务（裸指针）+ T 债务（残余原子化）
  ├─ L-1: 消除 g_backend
  ├─ L-2: ReActLoop 移动语义删除
  ├─ T-4/T-5/T-6: 字段原子化 + static_assert
  └─ 验收：ThreadSanitizer / 全量测试

Week 3~4: H 债务（HTTP 客户端）
  ├─ H-1: CURLSH 连接共享
  ├─ H-2: 总时长超时
  ├─ H-3: HttpRetryPolicy 提取
  ├─ H-4: URL 解析统一
  └─ 验收：压力测试（并发请求）、超时测试

Week 5: C 债务（配置系统）+ G 债务（日志）
  ├─ C-2: ConfigSchema
  ├─ C-3: ConfigScope DI 化
  ├─ C-4: 环境变量自动绑定
  ├─ G-2: Logger 析构 join
  ├─ G-3: 命名空间统一
  └─ G-4: get_instance() 返回引用

Week 6: D 债务（DI 深化）+ 收尾
  ├─ D-2: build_app() 工厂函数
  ├─ D-3: IBackendAdmin 拆分
  ├─ E: V2 前置准备（Error 类型设计）
  └─ 验收：集成测试 + 性能基准 + 文档更新
```

### 依赖关系

```
L 债务 ──┐
T 债务 ──┼──→ 全量测试通过 ──→ H 债务 ──→ 性能基准
C 债务 ──┤         ↑
G 债务 ──┘         └──→ D 债务 ──→ 集成测试
```

---

## 十、验收总纲

### 10.1 功能验收

- [ ] 所有现有单元测试通过（319 cases / 1096 assertions）
- [ ] 集成测试通过（需 LLM backend）
- [ ] TUI 交互无回归（输入、渲染、命令、模型切换）
- [ ] 非 TUI 模式（`Client`）正常工作

### 10.2 代码验收

- [ ] `grep -rn "\*\s*=" src/ | grep -v "//\|/\*"` 无裸指针解引用赋值（防御性检查）
- [ ] 新增代码无编译警告（/W4 或 -Wall -Wextra -Wpedantic 干净）
- [ ] 新增模块有对应单元测试，覆盖率不低于 80%

### 10.3 性能验收

- [ ] 并发 10 个请求总耗时较重构前无退化（±5% 内）
- [ ] 内存占用稳定，Valgrind/ASAN 无泄漏报告
- [ ] 日志系统高负载（1000 条/秒）下不丢日志

### 10.4 文档验收

- [ ] `plan/工具线程安全审计报告.md` 已产出（如 Phase 3 未完成）
- [ ] 本文档各 Week 完成后勾选并标注实际耗时
- [ ] 新增/修改的公共接口有 Doxygen 注释

---

*文档版本: 1.0*  
*关联文档: ARCH_ANALYSIS_REPORT.md, ARCH_REFACTOR_PLAN.md, ARCH_REFACTOR_PLAN_V2.md（待创建）*
