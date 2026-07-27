---
name: mydev:sse
description: SSEParser + NDJSONParser streaming parsers for C++20. Use when needing SSE, Server-Sent Events, NDJSON, streaming parser, incremental parsing, or HTTP streaming.
---

# SSEParser + NDJSONParser — 流式解析器

增量解析 SSE / NDJSON 流式数据，自动缓冲不完整数据块。

## 快速参考

### SSEParser

```cpp
#include "sse_parser.hpp"

SSEParser parser([](const SSEEvent& event) {
    // event.data   — 事件数据（最常用）
    // event.event  — 事件类型（可选）
    // event.id     — 事件 ID（可选）
    // event.retry  — 重连间隔 ms（可选）
    if (event.has_data()) process(event.data);
});

parser.parse(chunk1);  // 逐块喂入，可能不完整
parser.parse(chunk2);
parser.reset();
parser.get_event_count();
```

### NDJSONParser

```cpp
NDJSONParser parser([](const std::string& line) {
    // line 是一个完整的 JSON 行
    auto json = nlohmann::json::parse(line);
});

parser.parse(chunk);
parser.reset();
```

### 实际使用：HTTP 流式响应

```cpp
httplib::Client cli("http://api.example.com");
auto res = cli.Get("/stream", [&](const char* data, size_t len) {
    sse_parser.parse(std::string(data, len));
    return true;
});
```

## SSE 格式

```
data: {"content": "hello"}\n
\n                          ← 空行分隔事件
event: message\n
data: {"content": "world"}\n
\n
```

## 设计决策

| 要点 | SSEParser | NDJSONParser |
|------|-----------|--------------|
| 边界 | 双换行 `\n\n` 分隔事件 | 单换行 `\n` 分隔行 |
| 缓冲 | 不完整数据存 `m_buffer` | 不完整行存 `m_buffer` |
| 回调 | 传入 `SSEEvent` 结构 | 传入 `std::string` 行 |
| 换行 | 自动处理 `\r\n` / `\n` | 自动处理 `\r\n` / `\n` |
| 依赖 | 无 | 无 |

## 源码

- [src/sse_parser.hpp](src/sse_parser.hpp) — 头文件
- [src/sse_parser.cpp](src/sse_parser.cpp) — 实现文件
