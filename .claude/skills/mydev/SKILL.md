---
name: mydev
description: C++20 通用编码规范与可复用组件库。Use when writing or reviewing C++ code, or needing Result/EventBus/TaskManager/SSEParser/ConfigManager implementations. Triggers: C++ code, naming, RAII, smart pointers, error handling, async task, event bus, streaming parser, config.
---

# mydev — 按需加载索引

根据关键词加载对应子文件，**不要一次读取所有文件**。

## 基础规范

| 触发关键词 | 加载文件 |
|------------|----------|
| 命名、格式化、注释、头文件、clang-format | [style.md](style.md) |
| 智能指针、RAII、lambda、constexpr、内存、错误处理 | [cpp.md](cpp.md) |
| 测试、审查、性能、质量 | [quality.md](quality.md) |
| CMake、构建、编译选项 | [cmake.md](cmake.md) |

## 可复用组件（独立 skill，含源码）

| 触发关键词 | 组件 | 加载入口 |
|------------|------|----------|
| Result、unwrap、TRY_RESULT、错误处理类型 | Result\<T,E\> | [components/result/SKILL.md](components/result/SKILL.md) |
| EventBus、事件、订阅、发布、EventGuard | EventBus | [components/eventbus/SKILL.md](components/eventbus/SKILL.md) |
| Task、任务、异步、取消、进度、TaskManager | TaskManager | [components/task/SKILL.md](components/task/SKILL.md) |
| SSE、流式、NDJSON、解析器 | SSEParser | [components/sse/SKILL.md](components/sse/SKILL.md) |
| 配置、ConfigManager、ConfigScope | ConfigManager | [components/config/SKILL.md](components/config/SKILL.md) |

## 组件依赖关系

```
Result<T,E> ←── EventBus ←── TaskManager
     └─────────── ConfigManager
SSEParser / NDJSONParser（独立）
```

每个组件的 `src/` 目录包含可直接复制使用的源码文件。
