---
name: mydev:result
description: Result<T,E> Rust-style error handling for C++20. Use when needing Result, unwrap, TRY_RESULT, error handling types, or functional error propagation.
---

# Result\<T, E\> — Rust 风格错误处理

零堆分配的类型安全错误处理，替代异常做控制流。

## 快速参考

```cpp
#include "result.h"

// 构造
auto ok = Result<int, std::string>::ok(42);
auto err = Result<int, std::string>::err("失败");
auto ok_void = Result<void, std::string>::ok();

// 检查
r.isOk() / r.isErr()

// 取值
r.unwrap()        // 成功返回值，失败抛异常
r.unwrap_or(def)  // 成功返回值，失败返回默认值
r.error()         // 失败返回错误，成功抛异常

// 链式
r.map([](int v){ return v * 2; })
r.map_err([](string e){ return MyError(e); })
r.and_then([](int v){ return other(v); })

// 便捷宏
TRY_RESULT(some_operation());       // 快速传播错误
UNWRAP_RESULT(val, some_operation()); // 解包并绑定

// 类型特征
static_assert(is_result_v<Result<int, string>>);
```

## 使用模式

```cpp
// 1. 函数返回 Result
Result<void, std::string> on_load() {
    if (!init()) return Result<void, std::string>::err("初始化失败");
    return Result<void, std::string>::ok();
}

// 2. 链式调用
auto result = load_config()
    .and_then([&](auto cfg){ return apply_config(cfg); })
    .map_err([](auto e){ return "Config: " + e; });
```

## 源码

- [src/result.h](src/result.h) — 头文件，header-only，直接复制即用
