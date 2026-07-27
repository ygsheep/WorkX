# 架构重构方案 V2：错误处理统一

> **项目代号**：ARCH_REFACTOR_PLAN_V2
> **启动时机**：V1（架构重构）完成后启动
> **目标**：统一 4 种并存的错误处理风格为 `ResultV2<T>` + `Error` 类型
> **覆盖问题**：E-1（风格并存）/ E-2（unwrap 抛异常）/ E-3（send_message 返回 void）
> **文档版本**：1.0（初稿）
> **基于代码版本**：2026-07-27（feature/tech-debt-cleanup 分支 commit 28a6813）

---

## 一、背景与动机

### 1.1 问题诊断

当前代码存在 **4 种错误处理风格并存**（详见 `ARCH_ANALYSIS_REPORT.md` §3.8）：

| 风格 | 使用场景 | 代表签名 |
|------|----------|----------|
| `Result<T, E>` | 配置/Backend/Client/工具权限 | `Result<void, std::string> load_from_file(...)` |
| `struct + bool` | 工具结果/流状态/ReAct 结果 | `ToolResult{is_error}` / `StreamState::Error` / `ReActResult{was_error}` |
| 异常 | Result 内部 + 第三方库接住 | `unwrap()` 抛 `std::runtime_error`；22 处 try/catch |
| `HttpResponse + string` | HTTP 客户端 | `HttpResponse{status_code, error}` |

### 1.2 核心痛点

1. **风格分裂增加心智负担**：同一模块内并存多种风格（如 `ChatSession`：`send_message()` 返回 void，`save_session()` 返回 Result）
2. **`unwrap()` 抛异常是反模式**（E-2）：6 处 throw 全在 Result 模板内，外部至少 1 处无 isOk 守卫调用（`model_selector.cpp:78`）
3. **`send_message()` 返回 void**（E-3）：同步调用方无法感知失败，错误通过 EventBus 异步传递，调用链不直观
4. **错误信息丢失上下文**：`Result<T, std::string>` 仅含字符串消息，无法区分错误码（网络超时 vs HTTP 4xx vs 权限拒绝），重试策略只能靠字符串匹配
5. **HTTP 错误桥接冗余**：`remote_backend.cpp::list_models` 手工把 `HttpResponse` 转换为 `Result`，每个调用点重复

### 1.3 V1 已完成的前置工作

- ✅ E-5：`ExecutionResult` 字段语义文档化 + `is_ok()`/`is_truncated()` 便捷方法（commit ef3d295）
- ✅ E-6：`HttpResponse` 新增 7 个便捷方法（`is_success`/`is_http_error`/`is_network_error` 等）（commit ef3d295）
- ✅ D-2/D-3：工厂化 main.cpp + IBackendAdmin 拆分（commit b8cbaf9）

V2 在此基础上进行风格统一，**不重复 E-5/E-6 的语义清晰化工作**。

---

## 二、设计目标

### 2.1 核心目标

1. **统一错误类型**：所有可失败 API 返回 `ResultV2<T>`，`Error` 类型携带错误码 + 消息 + 上下文
2. **消除 unwrap 抛异常**：用 `get_or()` / `map()` / `and_then()` 链式调用替代，`unwrap()` 标记 `[[deprecated]]`
3. **send_message 可失败感知**：返回 `ResultV2<void>` 或 `ResultV2<ReActResult>`，同时保留 EventBus 异步事件（兼容现有 UI）
4. **错误码驱动重试**：`HttpRetryPolicy` 基于 `Error::code` 判断，不再字符串匹配
5. **向后兼容**：旧 `Result<T, std::string>` 保留并标记 `[[deprecated]]`，过渡期共存

### 2.2 非目标

- ❌ 不引入 C++ 异常作为正常控制流（保持当前"异常仅用于真正异常情况"策略）
- ❌ 不重写第三方库异常处理（nlohmann::json::exception、std::filesystem::filesystem_error 仍用 try/catch 接住）
- ❌ 不改变 EventBus 异步错误事件流（StreamErrorEvent/StreamDoneEvent 保持）
- ❌ 不改变 TaskManager 的 Task 失败模型（TaskFailedEvent 保持）

---

## 三、Error 类型设计

### 3.1 Error 结构体

```cpp
// src/core/utils/error.h
#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

namespace agent {

/// @brief 统一错误类型（V2）
/// @details 携带错误码 + 消息 + 上下文，替代 Result<T, std::string> 中的字符串错误
struct Error {
    /// @brief 错误码枚举
    /// @details 按"错误来源"分类，不按 HTTP 状态码分类（避免业务层依赖 HTTP 细节）
    enum class Code : int {
        Ok = 0,                  ///< 成功（不应出现在 Error 中，仅用于内部断言）

        // === 网络类（1xx）===
        NetworkTimeout = 100,    ///< 网络超时（连接/读取/总时长）
        NetworkDisconnected = 101, ///< 连接断开（DNS 失败、TCP reset）
        NetworkUnreachable = 102,  ///< 网络不可达

        // === HTTP 类（2xx）===
        HttpError = 200,         ///< HTTP 4xx/5xx（具体状态码见 context）
        HttpRateLimited = 201,   ///< HTTP 429 限流
        HttpServerDown = 202,    ///< HTTP 5xx 服务端错误

        // === 输入类（3xx）===
        InvalidInput = 300,      ///< 输入参数无效（JSON 解析失败、类型不匹配）
        MissingArgument = 301,   ///< 缺少必填参数
        InvalidFormat = 302,     ///< 格式错误（如 URL 解析失败）

        // === 权限类（4xx）===
        PermissionDenied = 400,  ///< 权限拒绝（工具不允许执行）
        ResourceNotFound = 401,  ///< 资源不存在（文件/配置键/模型）
        AuthenticationFailed = 402, ///< 认证失败（API key 无效）

        // === 业务类（5xx）===
        Cancelled = 500,         ///< 操作被取消（用户中断/超时取消）
        InternalError = 501,     ///< 内部错误（不变量违反、不应到达的状态）
        NotImplemented = 502,    ///< 功能未实现（TODO 工具）
        ToolExecutionFailed = 503, ///< 工具执行失败（工具内部异常）

        // === 配置类（6xx）===
        ConfigInvalid = 600,     ///< 配置值无效（Schema 校验失败）
        ConfigMissing = 601,     ///< 配置键缺失
        ConfigParseFailed = 602, ///< 配置文件解析失败

        // === 流式类（7xx）===
        StreamError = 700,       ///< 流式传输错误（SSE 解析失败、连接中断）
        StreamCancelled = 701,   ///< 流式传输被取消

        // === 未知 ===
        Unknown = 999,
    } code = Code::Unknown;

    std::string message;          ///< 人类可读错误描述
    std::string context;          ///< 额外上下文（URL / 请求 ID / 工具名 / 配置键）

    /// @brief 是否可重试
    /// @details 网络超时、HTTP 429/5xx、临时不可达可重试；权限/输入错误不可重试
    [[nodiscard]] bool is_retryable() const noexcept {
        switch (code) {
            case Code::NetworkTimeout:
            case Code::NetworkDisconnected:
            case Code::NetworkUnreachable:
            case Code::HttpRateLimited:
            case Code::HttpServerDown:
            case Code::StreamError:
                return true;
            default:
                return false;
        }
    }

    /// @brief 是否为网络错误（HTTP 之外的传输层错误）
    [[nodiscard]] bool is_network_error() const noexcept {
        return code >= Code::NetworkTimeout && code <= Code::NetworkUnreachable;
    }

    /// @brief 是否为 HTTP 错误
    [[nodiscard]] bool is_http_error() const noexcept {
        return code >= Code::HttpError && code <= Code::HttpServerDown;
    }

    /// @brief 是否为客户端错误（4xx，通常不可重试）
    [[nodiscard]] bool is_client_error() const noexcept {
        return code == Code::PermissionDenied
            || code == Code::AuthenticationFailed
            || code == Code::ResourceNotFound;
    }

    /// @brief 错误码 → 字符串（用于日志）
    [[nodiscard]] std::string_view code_string() const noexcept;

    /// @brief 格式化为完整错误消息（包含 code + message + context）
    /// @details 例如 "[NetworkTimeout] Request timed out after 30000ms (url=https://api.example.com)"
    [[nodiscard]] std::string to_string() const;

    /// @brief 工厂：从 HttpResponse 构造错误
    /// @details 根据 status_code 和 error 字段映射到对应 Code
    static Error from_http_response(unsigned int status_code, const std::string& body, const std::string& curl_error);

    /// @brief 工厂：从 CURLcode 构造网络错误
    static Error from_curl_code(int curl_code, const std::string& url);

    /// @brief 工厂：从 std::exception 构造内部错误
    static Error from_exception(const std::exception& e, std::string_view context = {});
};

/// @brief Error 比较运算符（主要用于测试断言）
bool operator==(const Error& lhs, const Error& rhs) noexcept;
bool operator!=(const Error& lhs, const Error& rhs) noexcept;

} // namespace agent
```

### 3.2 ResultV2 模板

```cpp
// src/core/utils/result_v2.h
#pragma once

#include <variant>
#include <functional>
#include "core/utils/error.h"

namespace agent {

/// @brief 统一结果类型（V2）
/// @details std::variant<T, Error> 的语义化封装，替代 Result<T, std::string>
/// @note 不提供 unwrap() 抛异常路径；用 get_or/map/and_then 链式调用
template<typename T>
class ResultV2 {
    static_assert(!std::is_same_v<T, Error>, "ResultV2<Error> is not allowed");

public:
    /// @brief 成功构造
    static ResultV2 ok(T value) {
        ResultV2 r;
        r.m_data.template emplace<0>(std::move(value));
        return r;
    }

    /// @brief 错误构造
    static ResultV2 err(Error error) {
        ResultV2 r;
        r.m_data.template emplace<1>(std::move(error));
        return r;
    }

    /// @brief 错误构造（便捷：错误码 + 消息）
    static ResultV2 err(Error::Code code, std::string message, std::string context = {}) {
        return err(Error{.code = code, .message = std::move(message), .context = std::move(context)});
    }

    [[nodiscard]] bool is_ok() const noexcept { return m_data.index() == 0; }
    [[nodiscard]] bool is_err() const noexcept { return m_data.index() == 1; }

    /// @brief 获取值引用（仅 ok 时有效，调用方需先 is_ok() 守卫）
    /// @note 不抛异常，err 时为未定义行为（debug 模式 assert）
    [[nodiscard]] T& value() {
        // assert(is_ok() && "Called value() on error result");
        return std::get<0>(m_data);
    }
    [[nodiscard]] const T& value() const {
        return std::get<0>(m_data);
    }

    /// @brief 获取错误引用（仅 err 时有效）
    [[nodiscard]] Error& error() {
        return std::get<1>(m_data);
    }
    [[nodiscard]] const Error& error() const {
        return std::get<1>(m_data);
    }

    /// @brief 获取值或默认值（不抛异常）
    [[nodiscard]] T value_or(T default_value) const {
        return is_ok() ? std::get<0>(m_data) : std::move(default_value);
    }

    /// @brief 映射成功值（ functor ）
    template<typename F>
    auto map(F&& f) -> ResultV2<decltype(f(std::declval<T>()))> {
        using R = decltype(f(std::declval<T>()));
        if (is_ok()) {
            return ResultV2<R>::ok(f(std::get<0>(m_data)));
        }
        return ResultV2<R>::err(std::get<1>(m_data));
    }

    /// @brief 链式操作（monadic bind）
    template<typename F>
    auto and_then(F&& f) -> decltype(f(std::declval<T>())) {
        using R = decltype(f(std::declval<T>()));
        if (is_ok()) {
            return f(std::get<0>(m_data));
        }
        return R::err(std::get<1>(m_data));
    }

    /// @brief 错误映射（error functor）
    template<typename F>
    ResultV2 map_err(F&& f) {
        if (is_err()) {
            return ResultV2::err(f(std::get<1>(m_data)));
        }
        return *this;
    }

private:
    std::variant<T, Error> m_data;
};

/// @brief ResultV2<void> 特化
template<>
class ResultV2<void> {
public:
    static ResultV2 ok() {
        ResultV2 r;
        r.m_is_ok = true;
        return r;
    }
    static ResultV2 err(Error error) {
        ResultV2 r;
        r.m_is_ok = false;
        r.m_error = std::move(error);
        return r;
    }
    static ResultV2 err(Error::Code code, std::string message, std::string context = {}) {
        return err(Error{.code = code, .message = std::move(message), .context = std::move(context)});
    }

    [[nodiscard]] bool is_ok() const noexcept { return m_is_ok; }
    [[nodiscard]] bool is_err() const noexcept { return !m_is_ok; }

    [[nodiscard]] const Error& error() const { return m_error; }

    template<typename F>
    auto map(F&& f) -> ResultV2<decltype(f())> {
        using R = decltype(f());
        if (is_ok()) {
            return ResultV2<R>::ok(f());
        }
        return ResultV2<R>::err(m_error);
    }

    template<typename F>
    auto and_then(F&& f) -> decltype(f()) {
        using R = decltype(f());
        if (is_ok()) {
            return f();
        }
        return R::err(m_error);
    }

private:
    bool m_is_ok = true;
    Error m_error;
};

} // namespace agent
```

### 3.3 便捷工具

```cpp
// src/core/utils/result_v2.h（续）

/// @brief TRY 宏：错误传播（替代 unwrap）
/// @details auto r = some_op(); if (r.is_err()) return r.err();
///          简化为：TRY(auto r, some_op());
#define TRY_RESULT_V2(var, expr) \
    auto var = (expr); \
    if (var.is_err()) return std::move(var).error();

/// @brief 从旧 Result 转换到 ResultV2（过渡期桥接）
template<typename T>
ResultV2<T> to_result_v2(Result<T, std::string>&& old) {
    if (old.isOk()) {
        return ResultV2<T>::ok(std::move(old.unwrap()));
    }
    return ResultV2<T>::err(Error::Code::Unknown, old.error(), {});
}

/// @brief 从 ResultV2 转换到旧 Result（过渡期桥接，逆向）
template<typename T>
Result<T, std::string> to_result_old(ResultV2<T>&& v2) {
    if (v2.is_ok()) {
        return Result<T, std::string>::ok(std::move(v2.value()));
    }
    return Result<T, std::string>::err(v2.error().to_string());
}
```

---

## 四、迁移策略

### 4.1 兼容原则

1. **旧 `Result<T, std::string>` 保留**：标记 `[[deprecated("Use ResultV2<T>")]]`，过渡期至少 1 个版本
2. **新代码强制使用 `ResultV2`**：新模块/新接口禁止使用旧 `Result`
3. **提供双向桥接**：`to_result_v2()` / `to_result_old()` 函数（§3.3）
4. **`unwrap()` 标记 `[[deprecated]]`**：保留但提示用 `value()` 或 `value_or()` 替代
5. **渐进式迁移**：按模块逐个迁移，每个模块迁移后立即全量测试

### 4.2 错误码映射表

现有错误字符串 → Error::Code 映射规则：

| 现有错误来源 | 映射到 Error::Code | context 字段填充 |
|--------------|-------------------|------------------|
| `Result::err("HTTP request failed: ...")` | `NetworkTimeout` / `NetworkDisconnected` / `HttpError` | URL |
| `Result::err("HTTP error: 404 (...)")` | `ResourceNotFound` (404) / `HttpError` (其他 4xx) / `HttpServerDown` (5xx) | URL + status_code |
| `Result::err("Config key not found: ...")` | `ConfigMissing` | 配置键名 |
| `Result::err("Invalid type for ...")` | `ConfigInvalid` | 配置键名 + 期望类型 |
| `Result::err("Permission denied: ...")` | `PermissionDenied` | 工具名 |
| `Result::err("Tool not found: ...")` | `ResourceNotFound` | 工具名 |
| `Result::err("JSON parse error: ...")` | `InvalidInput` | JSON 片段 |
| `ToolResult::error("not implemented")` | `NotImplemented` | 工具名 |
| `ToolResult::error(其他)` | `ToolExecutionFailed` | 工具名 |
| `StreamState::Error` | `StreamError` | 会话 ID |
| `StreamState::Cancelled` | `StreamCancelled` | 会话 ID |
| `HttpResponse.is_network_error()` | 根据 CURL code 映射到 `NetworkTimeout` / `NetworkDisconnected` / `NetworkUnreachable` | URL + curl 错误消息 |
| `HttpResponse.is_rate_limited()` (429) | `HttpRateLimited` | URL |
| `HttpResponse.is_server_error()` (5xx) | `HttpServerDown` | URL + status_code |
| `HttpResponse.is_client_error()` (4xx) | `HttpError` (非 404/429) / `ResourceNotFound` (404) | URL + status_code |
| `std::exception` (兜底) | `InternalError` | 函数名 |

### 4.3 模块迁移优先级

按"影响面小 → 影响面大"顺序，降低回归风险：

| 阶段 | 模块 | 文件数 | 调用点数 | 优先级 | 依赖 |
|------|------|--------|----------|--------|------|
| **P0** | Error + ResultV2 类型 | 新增 2 | 0 | 高 | 无 |
| **P1** | 配置系统 | 4 | ~20 | 高 | P0 |
| **P2** | HttpClient + HttpResponse | 3 | ~15 | 高 | P0 |
| **P3** | Backend + Client | 5 | ~30 | 中 | P2 |
| **P4** | 工具系统（ToolResult/ExecutionResult） | 5 | ~25 | 中 | P0 |
| **P5** | ChatSession + send_message | 2 | ~10 | 中 | P3/P4 |
| **P6** | ReActLoop + ReActResult | 2 | ~15 | 中 | P5 |
| **P7** | TUI 平台层（Terminal/Platform） | 3 | ~5 | 低 | P0 |
| **P8** | 清理旧 Result + 标记 deprecated | 全量 | — | 低 | P1-P7 全部完成 |

---

## 五、分阶段实施计划

### Phase V2-0：Error + ResultV2 类型骨架（无业务改动）

**目标**：实现 Error 和 ResultV2 类型，添加完整单元测试，不迁移任何业务代码。

**改动范围**：
- 新增 `src/core/utils/error.h` + `error.cpp`
- 新增 `src/core/utils/result_v2.h`（header-only）
- 新增 `tests/unit/core/utils/test_error.cpp`（Error 类型测试）
- 新增 `tests/unit/core/utils/test_result_v2.cpp`（ResultV2 模板测试）
- 修改 `src/core/utils/CMakeLists.txt`（添加新源文件）
- 修改 `tests/unit/core/utils/CMakeLists.txt`（添加测试）

**验收标准**：
- [ ] Error 类型完整实现（构造/比较/便捷方法/工厂函数）
- [ ] ResultV2<T> 模板完整实现（ok/err/value/error/value_or/map/and_then/map_err）
- [ ] ResultV2<void> 特化完整实现
- [ ] 单元测试 ≥ 30 cases，覆盖所有公开 API
- [ ] 编译 0 警告（Windows + Linux）
- [ ] 现有 405 单元测试无回归

**预计规模**：~400 行实现 + ~300 行测试

---

### Phase V2-1：配置系统迁移

**目标**：`IConfigManager` 和 `ConfigManager` 接口全部返回 `ResultV2<T>`，旧接口标记 deprecated 转发。

**改动范围**：
- `src/core/config/i_config_manager.h`：8 个方法签名改为 `ResultV2`
- `src/core/config/config_manager.h/.cpp`：实现迁移，保留旧接口 `[[deprecated]]` 转发
- `src/core/config/config_schema.h`：`validate_value` 返回 `ResultV2<void>`
- `src/app/config/app_config.cpp`：调用方适配
- `src/app/config/cli_args.cpp`：调用方适配
- `src/app/ui/model_selector.cpp`：修复无守卫的 `unwrap()` 调用（E-2 隐患）
- `tests/unit/core/config/test_config_manager.cpp`：测试迁移

**验收标准**：
- [ ] 配置系统所有公开接口返回 `ResultV2`
- [ ] 旧 `Result` 接口保留并标记 `[[deprecated]]`
- [ ] `model_selector.cpp:78` 的无守卫 unwrap 修复
- [ ] 单元测试通过，新增 Error::Code 断言

---

### Phase V2-2：HttpClient + HttpResponse 迁移

**目标**：`HttpClient::get()` 返回 `ResultV2<HttpResponse>`，错误时携带 `Error::Code`；`async_post_stream()` 错误路径改为 `Error`。

**改动范围**：
- `src/agent/api/remote/http_client.h`：`get()` 签名改为 `ResultV2<HttpResponse, Error>`
- `src/agent/api/remote/http_client.cpp`：实现迁移，`from_curl_code()` / `from_http_response()` 工厂
- `src/agent/api/remote/remote_backend.cpp`：调用方适配，删除手工桥接代码
- `tests/unit/agent/api/test_http_response.cpp`：测试迁移

**验收标准**：
- [ ] HttpClient 接口返回 `ResultV2`
- [ ] `remote_backend.cpp::list_models` 删除手工 `HttpResponse → Result` 桥接
- [ ] `HttpRetryPolicy::is_retryable()` 基于 `Error::code` 判断
- [ ] 单元测试通过

---

### Phase V2-3：Backend + Client 迁移

**目标**：`IBackend` / `Client` 接口返回 `ResultV2`。

**改动范围**：
- `src/agent/api/i_backend.h` / `i_completion_provider.h` / `i_backend_admin.h`
- `src/agent/api/remote/remote_backend.h/.cpp`
- `src/agent/api/client.h/.cpp`：8 个 Result 签名迁移
- 调用方：`chat_session.cpp` / `model_selector.cpp` / `factory.cpp`

**验收标准**：
- [ ] Backend/Client 接口返回 `ResultV2`
- [ ] 集成测试通过（6 cases + 9 skipped）

---

### Phase V2-4：工具系统迁移

**目标**：`ToolResult` 和 `ExecutionResult` 风格统一，引入 `ToolError` 子类型。

**改动范围**：
- `src/agent/tool/result.h`：`ToolResult` 保留 struct，但 `is_error` 改为 `Error` 字段（可空）
- `src/agent/tool/executor.h`：`ExecutionResult` 重构为 `ResultV2<ToolResult>`
- `src/agent/tool/itool.h`：`PermissionResult` / `ValidationResult` 改为 `ResultV2<void>`
- `src/agent/tool/executor.cpp`：try/catch 转换为 `Error::from_exception()`
- 所有工具实现（FileRead/FileWrite/FileEdit/Glob/Grep/Bash/Agent/MCP/WebFetch）：适配

**验收标准**：
- [ ] 工具系统错误统一为 `Error` 类型
- [ ] `ToolExecutor::execute()` 返回 `ResultV2<ToolResult>`
- [ ] 5 路 try/catch 转为 `Error::from_exception()`
- [ ] 单元测试通过

---

### Phase V2-5：ChatSession + send_message 迁移

**目标**：`send_message()` 返回 `ResultV2<void>`，同步调用方可感知失败（E-3 修复）。

**改动范围**：
- `src/agent/core/chat_session.h/.cpp`：`send_message()` / `save_session()` / `load_session()` 返回 `ResultV2`
- `src/app/ui/`：调用方适配
- `tests/unit/agent/core/test_chat_session.cpp`：测试迁移

**验收标准**：
- [ ] `send_message()` 返回 `ResultV2<void>`
- [ ] EventBus 异步事件流保持兼容
- [ ] 调用方可选择同步检查错误或异步监听事件

---

### Phase V2-6：ReActLoop 迁移

**目标**：`ReActResult` 的 `was_error` / `error_message` 改为 `Error` 字段。

**改动范围**：
- `src/agent/core/react_loop.h/.cpp`：`ReActResult` 重构
- `src/agent/core/chat_session.cpp`：调用方适配（重试判定基于 `Error::code`）
- `tests/unit/agent/core/test_react_loop.cpp`：测试迁移

**验收标准**：
- [ ] `ReActResult` 携带 `Error` 字段
- [ ] `ChatSession` 重试判定基于 `Error::is_retryable()`

---

### Phase V2-7：TUI 平台层迁移

**目标**：`Terminal::initialize()` / `IPlatform::enable_raw_mode()` 返回 `ResultV2`。

**改动范围**：
- `src/tui/core/terminal.h/.cpp`
- `src/tui/core/platform/i_platform.h`
- `src/tui/core/platform/platform_win32.cpp` / `platform_posix.cpp`
- `src/app/factory.cpp`：调用方适配

**验收标准**：
- [ ] TUI 平台层接口返回 `ResultV2`
- [ ] 跨平台测试通过

---

### Phase V2-8：清理旧 Result + 标记 deprecated

**目标**：所有业务代码迁移完成后，正式标记旧 `Result` 为 deprecated，准备下版本删除。

**改动范围**：
- `src/core/utils/result.h`：`Result` 类标记 `[[deprecated]]`，`unwrap()` 标记 `[[deprecated]]`
- 全量代码审查，确保无新代码使用旧 `Result`
- 更新文档

**验收标准**：
- [ ] 旧 `Result` 全部标记 deprecated
- [ ] 编译时 deprecated 警告仅出现在过渡桥接代码
- [ ] 全量测试通过
- [ ] `TECH_DEBT_REGISTRY.md` 标记 E-1/E-2/E-3 已修复

---

## 六、向后兼容策略

### 6.1 过渡期共存

```cpp
// 旧接口（deprecated 但保留）
class ConfigManager {
public:
    [[deprecated("Use get_value_v2")]]
    Result<ConfigValue, std::string> get_value(const std::string& key) const;

    // 新接口
    ResultV2<ConfigValue> get_value_v2(const std::string& key) const;
};

// 桥接实现
inline ResultV2<ConfigValue> ConfigManager::get_value_v2(const std::string& key) const {
    return to_result_v2(get_value(key));  // 复用旧实现
}
```

### 6.2 调用方适配

```cpp
// 旧调用方（不修改，触发 deprecated 警告）
auto r = cfg.get_value("key");
if (r.isOk()) { /* use r.unwrap() */ }

// 新调用方
auto r = cfg.get_value_v2("key");
if (r.is_ok()) { /* use r.value() */ }
else { /* r.error().code, r.error().message */ }
```

### 6.3 unwrap 处理

```cpp
// 旧（deprecated）
auto v = r.unwrap();  // 可能抛异常

// 新（不抛异常）
auto v = r.value();          // 需先 is_ok() 守卫，否则 UB
auto v = r.value_or(default); // 安全
auto v = r.map([](auto x){return x*2;}).value_or(0);
```

---

## 七、测试策略

### 7.1 单元测试覆盖

每个 Phase 必须新增/迁移对应测试：
- Error 类型：≥ 15 cases（构造/比较/便捷方法/工厂/from_http_response/from_curl_code/from_exception）
- ResultV2<T>：≥ 15 cases（ok/err/value/error/value_or/map/and_then/map_err/TRY 宏）
- ResultV2<void>：≥ 5 cases（特化行为）
- 迁移模块：现有测试迁移 + 新增 Error::Code 断言

### 7.2 回归测试

每个 Phase 完成后：
- Windows + Linux 双平台编译
- 全量单元测试通过（≥ 405 cases）
- 集成测试通过（6 cases）
- 编译 0 警告

### 7.3 性能验证

- `bench_config_manager` / `bench_event_bus` 无回归
- 新增 `bench_error` / `bench_result_v2`（可选）

---

## 八、风险评估

### 8.1 高风险点

1. **`send_message()` 签名变更**：影响 TUI 事件流，需确保 EventBus 异步事件保持兼容
2. **`ToolExecutor::execute()` 返回类型变更**：影响 ReActLoop 调用链
3. **`HttpClient::get()` 签名变更**：影响所有 HTTP 调用方

### 8.2 缓解措施

- 每个 Phase 独立提交，便于回滚
- 保留旧接口 + deprecated 转发，过渡期共存
- 关键 Phase（V2-3/V2-5）需手动 TUI 交互验证

### 8.3 退场策略

如果迁移中发现重大问题：
1. 立即停止当前 Phase
2. 回滚到上一个稳定 commit
3. 重新评估方案
4. 调整后重试

---

## 九、验收总纲

### 9.1 功能验收

- [ ] 所有可失败 API 返回 `ResultV2<T>`
- [ ] `Error` 类型携带错误码 + 消息 + 上下文
- [ ] `unwrap()` 全部标记 deprecated，无新增调用
- [ ] `send_message()` 返回 `ResultV2<void>`（E-3 修复）
- [ ] `HttpRetryPolicy` 基于 `Error::code` 判断（E-1 修复）

### 9.2 代码验收

- [ ] 业务代码无 `throw`（仅 Result 内部保留 deprecated throw）
- [ ] 第三方库异常全部用 try/catch 接住并转为 `Error::from_exception()`
- [ ] 编译 0 警告（Windows + Linux）
- [ ] 旧 `Result` 标记 `[[deprecated]]`

### 9.3 测试验收

- [ ] 单元测试 ≥ 435 cases（405 + 30 新增）
- [ ] 集成测试通过
- [ ] 性能基准无回归

### 9.4 文档验收

- [ ] `TECH_DEBT_REGISTRY.md` 标记 E-1/E-2/E-3 已修复
- [ ] `ARCH_REFACTOR_PLAN_V2.md` 更新为完成状态
- [ ] 关键 API 有 Doxygen 注释

---

## 十、实施路线图

```
Phase V2-0 (Error + ResultV2 类型) ──┐
                                      ├─ Phase V2-1 (配置系统)
                                      ├─ Phase V2-2 (HttpClient)
                                      ├─ Phase V2-4 (工具系统)
                                      │
                                      └─ Phase V2-3 (Backend+Client) ──┐
                                                                         ├─ Phase V2-5 (ChatSession)
                                                                         │    ↓
                                                                         └─ Phase V2-6 (ReActLoop)
                                                                              ↓
                                                              Phase V2-7 (TUI 平台层)
                                                                              ↓
                                                              Phase V2-8 (清理 + deprecated)
```

**依赖关系**：
- V2-0 是所有后续 Phase 的前置
- V2-3 依赖 V2-2（Backend 调用 HttpClient）
- V2-5 依赖 V2-3 和 V2-4（ChatSession 调用 Backend 和工具）
- V2-6 依赖 V2-5（ReActLoop 被 ChatSession 调用）
- V2-8 依赖所有前置 Phase 完成

---

*文档版本: 1.0（初稿）*
*创建日期: 2026-07-27*
*关联文档: ARCH_REFACTOR_PLAN.md (V1，已完成), TECH_DEBT_REGISTRY.md (E-1/E-2/E-3 登记)*
