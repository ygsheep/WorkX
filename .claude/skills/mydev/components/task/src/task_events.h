/**
 * @file task_events.h
 * @brief 任务生命周期事件定义
 * @details 用于 TaskManager 与 EventBus 的集成，可选使用
 * @version 1.0.0
 *
 * 使用方式：需要 EventBus 集成时复制此文件
 * 在 task_manager.cpp 中取消注释 #include "task_events.h" 和相关 publish_async 调用
 */

#pragma once

#include <string>
#include <memory>

namespace mydev {

class Task;

/// @brief 任务开始事件
struct TaskStartedEvent {
    std::shared_ptr<Task> task;
    std::string task_name;
};

/// @brief 任务进度更新事件
struct TaskProgressEvent {
    std::shared_ptr<Task> task;
    std::string task_name;
    float progress;
    float progress_percent;  ///< 0.0 - 100.0
};

/// @brief 任务完成事件
struct TaskCompletedEvent {
    std::shared_ptr<Task> task;
    std::string task_name;
    float duration_ms;       ///< 执行耗时（毫秒）
};

/// @brief 任务失败事件
struct TaskFailedEvent {
    std::shared_ptr<Task> task;
    std::string task_name;
    std::string error_message;
    float duration_ms;
};

/// @brief 任务取消事件
struct TaskCancelledEvent {
    std::shared_ptr<Task> task;
    std::string task_name;
    float duration_ms;
};

} // namespace mydev
