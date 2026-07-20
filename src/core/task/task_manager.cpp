/**
 * @file task_manager.cpp
 * @brief 异步任务管理器实现
 */

#include "core/task/task_manager.h"
#include "core/events/event_bus.h"
#include "core/task/task_events.h"
#include <algorithm>
#include <thread>

namespace agent {

Task::Task(std::string name, TaskFunc func, float max_progress)
    : m_name(std::move(name))
    , m_func(std::move(func))
    , m_max_progress(max_progress)
{
}

Task::~Task() = default;

void Task::execute() {
    if (m_status != TaskStatus::Pending) return;

    m_status = TaskStatus::Running;
    m_start_time = std::chrono::steady_clock::now();

    const auto task_ptr = shared_from_this();

    EventBus::instance().publish_async(TaskStartedEvent{
        .task = task_ptr,
        .task_name = m_name
    });

    try {
        m_func(m_should_cancel);

        if (!m_should_cancel) {
            markCompleted();
        } else {
            m_status = TaskStatus::Cancelled;

            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - m_start_time
            ).count();

            EventBus::instance().publish_async(TaskCancelledEvent{
                .task = task_ptr,
                .task_name = m_name,
                .duration_ms = static_cast<float>(duration)
            });
        }
    } catch (const std::exception& e) {
        markFailed(e.what());
    } catch (...) {
        markFailed("Unknown exception");
    }
}

void Task::markCompleted() {
    m_progress = m_max_progress;
    m_status = TaskStatus::Completed;

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - m_start_time
    ).count();

    auto task_ptr = shared_from_this();

    EventBus::instance().publish_async(TaskCompletedEvent{
        .task = task_ptr,
        .task_name = m_name,
        .duration_ms = static_cast<float>(duration)
    });

    if (m_completed_callback) {
        m_completed_callback();
    }
}

void Task::markFailed(const std::string& error_message) {
    m_status = TaskStatus::Failed;

    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - m_start_time
    ).count();

    auto task_ptr = shared_from_this();

    EventBus::instance().publish_async(TaskFailedEvent{
        .task = task_ptr,
        .task_name = m_name,
        .error_message = error_message,
        .duration_ms = static_cast<float>(duration)
    });
}

TaskManager::~TaskManager() {
    cancelAll();
    waitForAll();
    // join 所有线程，防止 detach 导致 use-after-free
    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    for (auto& entry : m_entries) {
        if (entry.thread.joinable()) {
            entry.thread.join();
        }
    }
    m_entries.clear();
}

std::shared_ptr<Task> TaskManager::create(
    const std::string& name,
    Task::TaskFunc func,
    TaskType type
) {
    auto task = std::make_shared<Task>(name, std::move(func), 100.0f);
    task->setType(type);
    return task;
}

std::shared_ptr<Task> TaskManager::launch(
    const std::string& name,
    Task::TaskFunc func,
    TaskType type
) {
    auto task = create(name, std::move(func), type);
    start(task);
    return task;
}

void TaskManager::start(std::shared_ptr<Task> task) {
    if (!task) return;

    if (task->getType() == TaskType::Blocking) {
        task->execute();
        return;
    }

    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    auto& entry = m_entries.emplace_back();
    entry.task = task;
    entry.thread = std::thread([task]() {
        task->execute();
        // 通知 waitForAll 检查 predicate
        TaskManager::instance().m_tasks_cv.notify_all();
    });
}

void TaskManager::cancel(std::shared_ptr<Task> task) {
    if (!task) return;
    task->cancel();
}

std::vector<std::shared_ptr<Task>> TaskManager::getTasks() const {
    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    std::vector<std::shared_ptr<Task>> tasks;
    tasks.reserve(m_entries.size());
    for (const auto& entry : m_entries) {
        tasks.push_back(entry.task);
    }
    return tasks;
}

std::vector<std::shared_ptr<Task>> TaskManager::getRunningTasks() const {
    std::lock_guard<std::mutex> lock(m_tasks_mutex);

    std::vector<std::shared_ptr<Task>> running;
    for (const auto& entry : m_entries) {
        if (entry.task->isRunning()) {
            running.push_back(entry.task);
        }
    }
    return running;
}

void TaskManager::update() {
    std::lock_guard<std::mutex> lock(m_tasks_mutex);

    for (auto it = m_entries.begin(); it != m_entries.end(); ) {
        // Critical 类型不清理（持久任务）
        // 仅清理已 finished 的 entry，join 其线程
        if (it->task->isFinished() && it->task->getType() != TaskType::Critical) {
            if (it->thread.joinable()) {
                it->thread.join();
            }
            it = m_entries.erase(it);
        } else {
            ++it;
        }
    }
}

void TaskManager::waitForAll() {
    std::unique_lock<std::mutex> lock(m_tasks_mutex);
    // 30 秒兜底，防止 task 不响应 cancel 而永久阻塞
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    m_tasks_cv.wait_until(lock, deadline, [this]() {
        return m_entries.empty() || std::all_of(m_entries.begin(), m_entries.end(),
            [](const TaskEntry& e) { return e.task->isFinished(); });
    });
}

void TaskManager::cancelAll() {
    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    for (auto& entry : m_entries) {
        if (entry.task->isRunning()) {
            entry.task->cancel();
        }
    }
}

size_t TaskManager::getRunningTaskCount() const {
    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    return std::count_if(m_entries.begin(), m_entries.end(),
        [](const TaskEntry& entry) {
            return entry.task->isRunning();
        });
}

} // namespace agent
