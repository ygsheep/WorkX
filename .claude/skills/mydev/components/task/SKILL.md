---
name: mydev:task
description: TaskManager + Task async task system for C++20. Use when needing async tasks, task cancellation, progress tracking, TaskManager, or background work.
---

# TaskManager + Task — 异步任务系统

协作式取消、进度跟踪、线程安全的异步任务管理。

## 快速参考

```cpp
#include "task_manager.h"

// 创建并立即启动
auto task = TaskManager::instance().launch("任务名",
    [](const std::atomic<bool>& should_cancel) {
        for (int i = 0; i < 100; ++i) {
            if (should_cancel) return;  // 协作式取消
            // ... 做事
        }
    },
    TaskType::Normal  // Normal / Background / Blocking / Critical
);

// 创建不启动
auto task = TaskManager::instance().create("任务名", func);
TaskManager::instance().start(task);

// 进度
task->setProgress(50.0f);
task->addProgress(10.0f);
task->getProgressPercent();  // 0.0 ~ 1.0

// 取消
task->cancel();
task->shouldCancel();  // 原子检查

// 状态
task->isFinished();  // Completed | Cancelled | Failed
task->isRunning();

// 完成回调
task->onCompleted([]() { /* ... */ });

// 主循环更新（清理已完成任务）
TaskManager::instance().update();

// 批量
TaskManager::instance().cancelAll();
TaskManager::instance().waitForAll();
```

## 事件集成（可选）

如果同时使用 EventBus，任务生命周期会自动发布事件：
- `TaskStartedEvent` / `TaskCompletedEvent` / `TaskCancelledEvent` / `TaskFailedEvent` / `TaskProgressEvent`

在 `task_manager.cpp` 中取消注释 `publish_async` 调用即可启用。

## 设计决策

| 决策 | 原因 |
|------|------|
| `atomic<bool>` 协作式取消 | C++ 无法安全强制终止线程 |
| `shared_ptr<Task>` | 任务可能被多线程引用 |
| `enable_shared_from_this` | Task 内部需获取自身 shared_ptr |
| 主循环 `update()` | 避免在 worker 线程中操作主线程数据 |
| `detach` 线程 | 简单可靠；TaskManager 析构时 cancelAll + waitForAll |

## 依赖

- [mydev:eventbus](../eventbus/) — 可选，启用事件通知时需要

## 源码

- [src/task_manager.h](src/task_manager.h) — 头文件
- [src/task_manager.cpp](src/task_manager.cpp) — 实现文件
- [src/task_events.h](src/task_events.h) — 事件定义（可选，EventBus 集成时使用）
