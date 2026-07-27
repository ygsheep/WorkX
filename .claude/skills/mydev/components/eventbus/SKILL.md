---
name: mydev:eventbus
description: Type-safe EventBus + EventGuard RAII subscription manager for C++20. Use when needing EventBus, event subscription, publish/subscribe, EventGuard, or decoupled communication.
---

# EventBus + EventGuard — 类型安全事件总线

编译时类型安全的事件发布/订阅系统，RAII 守卫自动取消订阅。

## 快速参考

```cpp
#include "event_bus.h"

// 定义事件
struct WindowCloseEvent { int window_id; };

// 订阅（返回 Token）
auto token = EventBus::instance().subscribe<WindowCloseEvent>(
    [](const WindowCloseEvent& e) { /* 处理 */ }
);

// 取消订阅
EventBus::instance().unsubscribe<WindowCloseEvent>(token);

// 同步发布
EventBus::instance().publish(WindowCloseEvent{ .window_id = 42 });

// 异步发布（入队，需手动刷新）
EventBus::instance().publish_async(MyEvent{...});
EventBus::instance().process_async_events();  // 主循环中调用

// RAII 守卫（析构自动取消订阅）
EventGuard<MyEvent> guard([](const MyEvent& e) { /* 处理 */ });

// 便捷函数
auto guard = make_event_guard<MyEvent>([](const MyEvent& e) { /* 处理 */ });
```

## 批量管理

```cpp
class MyPlugin {
    std::vector<EventToken> m_tokens;
    void on_load() {
        m_tokens.push_back(EventBus::instance().subscribe<EventA>([...]{...}));
        m_tokens.push_back(EventBus::instance().subscribe<EventB>([...]{...}));
    }
    void on_unload() {
        // 逐个取消或 clear
        for (auto& t : m_tokens) { /* unsubscribe */ }
        m_tokens.clear();
    }
};
```

## 设计决策

| 决策 | 原因 |
|------|------|
| `recursive_mutex` | 同步 publish 可能在回调中再次 publish |
| Token 机制 | 比 raw pointer 安全，支持 invalidate |
| 异步队列 | worker 线程不直接访问主线程数据 |
| 锁内调用回调 | `recursive_mutex` 允许重入；回调中不要再操作 EventBus |

## 依赖

- [mydev:result](../result/) — 可选，EventBus 自身不依赖 Result

## 源码

- [src/event_bus.h](src/event_bus.h) — header-only，直接复制即用
