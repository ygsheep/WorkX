/**
 * @file task_manager.cpp
 * @brief 异步任务管理器实现
 */

#include "core/task/task_manager.h"
#include "core/events/event_bus.h"
#include "core/task/task_events.h"
#include <algorithm>
#include <thread>

namespace workx {

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

    auto task_ptr = shared_from_this();

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
}

std::shared_ptr<Task> TaskManager::create(
    const std::string& name,
    Task::TaskFunc func,
    TaskType type
) {
    auto task = std::make_shared<Task>(name, std::move(func), 100.0f);
    task->setType(type);

    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    m_tasks.push_back(task);

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
    } else {
        std::thread([task]() {
            task->execute();
        }).detach();
    }
}

void TaskManager::cancel(std::shared_ptr<Task> task) {
    if (!task) return;
    task->cancel();
}

std::vector<std::shared_ptr<Task>> TaskManager::getRunningTasks() const {
    std::lock_guard<std::mutex> lock(m_tasks_mutex);

    std::vector<std::shared_ptr<Task>> running;
    for (const auto& task : m_tasks) {
        if (task->isRunning()) running.push_back(task);
    }
    return running;
}

void TaskManager::update() {
    std::lock_guard<std::mutex> lock(m_tasks_mutex);

    auto it = std::remove_if(m_tasks.begin(), m_tasks.end(),
        [](const std::shared_ptr<Task>& task) {
            if (task->getType() == TaskType::Critical) return false;
            return task->isFinished();
        });

    if (it != m_tasks.end()) {
        m_tasks.erase(it, m_tasks.end());
    }
}

void TaskManager::waitForAll() {
    while (true) {
        {
            std::lock_guard<std::mutex> lock(m_tasks_mutex);
            bool all_finished = std::all_of(m_tasks.begin(), m_tasks.end(),
                [](const std::shared_ptr<Task>& task) {
                    return task->isFinished();
                });

            if (all_finished || m_tasks.empty()) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void TaskManager::cancelAll() {
    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    for (auto& task : m_tasks) {
        if (task->isRunning()) task->cancel();
    }
}

size_t TaskManager::getRunningTaskCount() const {
    std::lock_guard<std::mutex> lock(m_tasks_mutex);
    return std::count_if(m_tasks.begin(), m_tasks.end(),
        [](const std::shared_ptr<Task>& task) {
            return task->isRunning();
        });
}

} // namespace workx
