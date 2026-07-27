# Boost.Beast 替换 libcurl 方案

## Context

当前项目 `workx` 是一个 C++20 TUI 聊天客户端，使用 libcurl 发送 HTTP 请求到 OpenAI/Anthropic API。所有 curl 代码集中在 `src/backend/remote/remote_backend.cpp` 一个文件中，使用 curl easy 接口 + detached thread 模式。

**替换原因**：引入 Boost.Asio/Beast 统一异步 I/O 模型，消除 detached thread + 手动同步，为后续功能（如 WebSocket、多连接复用）打好基础。

## 核心设计

### 新增文件

| 文件 | 职责 |
|------|------|
| `src/backend/remote/http_client.h` | HTTP 客户端类声明 |
| `src/backend/remote/http_client.cpp` | HTTP 客户端实现（Beast 封装） |

### 修改文件

| 文件 | 变更 |
|------|------|
| `src/backend/remote/remote_backend.cpp` | 移除所有 curl 代码，改用 `HttpClient` |
| `src/backend/remote/remote_backend.h` | 移除 curl 相关 include，新增 `io_context` 和 `HttpClient` 成员 |
| `CMakeLists.txt` | 替换 `WORKX_USE_CURL` → `WORKX_USE_BEAST`，链接 Boost，移除 DLL 复制逻辑 |
| `vcpkg.json` | 移除 `curl`，新增 `boost-beast` 和 `boost-asio` |

### 不变的文件

- `sse_stream_reader.h/cpp` — 已经与传输层解耦，`feed_data()` / `finish()` 接口不变
- `i_provider_adapter.h` — `build_headers()` / `build_url()` / `build_request_body()` 不变
- `openai_adapter.h/cpp` / `anthropic_adapter.h/cpp` — 纯协议逻辑，不涉及 HTTP 传输
- 所有其他 `backend/`、`session/`、`tui/`、`core/` 文件

---

## HttpClient 类设计

```cpp
// src/backend/remote/http_client.h

#pragma once

#include <string>
#include <vector>
#include <utility>
#include <functional>
#include <memory>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>

namespace workx {

/// @brief HTTP 响应
struct HttpResponse {
    int status_code = 0;
    std::string body;
    std::string error;  // 非空表示连接/协议错误
};

/// @brief HTTP 客户端（基于 Boost.Beast）
class HttpClient {
public:
    explicit HttpClient(boost::asio::io_context& ioc);
    ~HttpClient();

    /// @brief 同步 GET 请求
    HttpResponse get(const std::string& url,
                     const std::vector<std::pair<std::string, std::string>>& headers,
                     int timeout_ms = 15000);

    /// @brief 异步流式 POST 请求
    /// @param on_data 收到数据块时回调，调用 reader->feed_data()
    /// @param on_done 请求结束时回调，调用 reader->finish()
    void async_post_stream(const std::string& url,
                           const std::vector<std::pair<std::string, std::string>>& headers,
                           const std::string& body,
                           std::function<void(const std::string&)> on_data,
                           std::function<void(const std::string&)> on_done,
                           int timeout_ms = 30000);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace workx
```

### 关键实现细节

**1. URL 解析**
- Beast 不解析 URL，需要手动从 `https://host:port/path` 提取 scheme/host/port/target
- 用 `boost::urls::parse_uri()` (Boost.URL) 或手写简易解析

**2. HTTPS (SSL)**
- 通过 `boost::asio::ssl::context` + `boost::beast::ssl_stream<tcp_stream>` 处理
- SSL context 默认使用系统 CA 证书（`ssl::context::sslv23_client`）
- `set_verify_mode(ssl::verify_peer)` 启用验证
- 当前 `list_models()` 禁用了 SSL 验证，保留为可配置选项

**3. 流式读取 (async_post_stream)**
- 使用 `beast::http::async_read_some()` 逐块读取响应体
- 每读一块，调用 `on_data(chunk)` → `reader->feed_data(chunk)`
- 读取完成或出错时调用 `on_done(error)`
- 在 io_context 的线程中执行异步操作

**4. io_context 线程模型**
- `RemoteBackend` 持有 `boost::asio::io_context` 和一个 `std::thread`
- 初始化时启动后台线程 `ioc.run()`
- 关闭时 `ioc.stop()` + join 线程
- 替换当前 `std::thread(...).detach()` 模式

**5. 同步 GET (list_models)**
- 使用 `beast::http::read()` 同步读取完整响应
- 在调用线程上直接执行（与当前行为一致）
- 复用同一个 `io_context`（同步操作不需要 io_context 运行）

**6. 超时**
- 使用 `beast::tcp_stream::expires_after()` 设置超时
- 超时触发 `beast::error::timeout`

---

## RemoteBackend 变更

### remote_backend.h 变更

```diff
+ #include <boost/asio/io_context.hpp>
+ #include "backend/remote/http_client.h"

  class RemoteBackend : public IBackend {
  private:
+     boost::asio::io_context m_ioc;
+     std::unique_ptr<std::thread> m_ioc_thread;
+     std::unique_ptr<HttpClient> m_http_client;
  };
```

### remote_backend.cpp 变更

**initialize()**:
```diff
- static std::once_flag curl_init_flag;
- std::call_once(curl_init_flag, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
+ m_http_client = std::make_unique<HttpClient>(m_ioc);
+ m_ioc_thread = std::make_unique<std::thread>([this]() { m_ioc.run(); });
```

**shutdown()**:
```diff
+ if (m_ioc_thread) {
+     m_ioc.stop();
+     m_ioc_thread->join();
+     m_ioc_thread.reset();
+ }
```

**submit_completion()**:
- 移除 `curl_easy_init/curl_easy_setopt/curl_easy_perform` 全部代码
- 改为：
```cpp
m_http_client->async_post_stream(
    url, header_pairs, body,
    [reader](const std::string& chunk) { reader->feed_data(chunk); },
    [reader](const std::string& error) { reader->finish(error); },
    m_config.timeout_ms
);
```

**list_models()**:
- 移除所有 curl 代码
- 改为：
```cpp
auto resp = m_http_client->get(url, header_pairs, 15000);
if (!resp.error.empty()) {
    return Result<...>::err(resp.error);
}
if (resp.status_code >= 400) {
    return Result<...>::err(std::format("HTTP error: {} ({})", resp.status_code, resp.body));
}
// 后续 JSON 解析逻辑不变
```

**移除**:
- `CurlCallbackData` 结构体
- `curl_write_callback` 函数
- `build_curl_slist` 函数
- `safe_curl_perform` 函数（SEH 不再需要）

---

## CMake 变更

```diff
- option(WORKX_USE_CURL "Enable CURL for remote backend" OFF)
- if(WORKX_USE_CURL)
-     find_package(CURL CONFIG REQUIRED)
- endif()

+ option(WORKX_USE_BEAST "Enable Boost.Beast for remote backend" OFF)
+ if(WORKX_USE_BEAST)
+     find_package(Boost REQUIRED COMPONENTS beast)
+ endif()

  # WORKX_lib 链接
- if(WORKX_USE_CURL)
-     target_link_libraries(WORKX_lib PUBLIC CURL::libcurl)
-     target_compile_definitions(WORKX_lib PUBLIC WORKX_HAS_CURL)
- endif()

+ if(WORKX_USE_BEAST)
+     target_link_libraries(WORKX_lib PUBLIC Boost::beast Boost::asio)
+     target_compile_definitions(WORKX_lib PUBLIC WORKX_HAS_BEAST)
+ endif()

  # 移除 DLL 复制逻辑（Boost 静态链接或 vcpkg 自动处理）
- if(WORKX_USE_CURL AND MSVC)
-     ...DLL 复制逻辑...
- endif()
```

## vcpkg.json 变更

```diff
  "dependencies": [
      "nlohmann-json",
-     "curl",
+     "boost-beast",
+     "boost-asio",
+     "boost-url",
      "catch2"
  ]
```

## 编译宏变更

- `WORKX_USE_CURL` → `WORKX_USE_BEAST`
- `WORKX_HAS_CURL` → `WORKX_HAS_BEAST`
- `remote_backend.cpp` 中 `#ifdef WORKX_HAS_CURL` → `#ifdef WORKX_HAS_BEAST`

---

## 实施步骤

1. **vcpkg.json** — 替换依赖 `curl` → `boost-beast` + `boost-asio` + `boost-url`
2. **CMakeLists.txt** — 替换 `WORKX_USE_CURL` → `WORKX_USE_BEAST`，更新 find_package 和链接
3. **新建 http_client.h** — 声明 `HttpClient` 类和 `HttpResponse` 结构
4. **新建 http_client.cpp** — 实现 Beast HTTP 客户端
   - URL 解析（scheme/host/port/target）
   - 同步 GET（含 SSL）
   - 异步流式 POST（含 SSL + 分块读取）
   - 超时处理
5. **修改 remote_backend.h** — 替换 curl 依赖为 `HttpClient` + `io_context`
6. **修改 remote_backend.cpp** — 移除所有 curl 代码，改用 `HttpClient`
7. **编译验证** — `cmake -DWORKX_USE_BEAST=ON && cmake --build .`
8. **功能验证** — 运行 workx 连接实际 API 测试流式/非流式请求

## 验证

1. **编译通过**：`cmake --build build --config Release` 无错误
2. **流式请求**：运行 workx，发送消息，验证 SSE 流式响应正常显示
3. **模型列表**：运行 `/model` 命令，验证 GET 请求返回模型列表
4. **中断**：生成中按 Ctrl+C，验证中断功能正常
5. **HTTPS**：验证与 `https://api.openai.com` 的 SSL 连接正常
6. **超时**：配置短超时，验证超时错误信息正确
7. **测试通过**：`ctest --test-dir build` 无回归
