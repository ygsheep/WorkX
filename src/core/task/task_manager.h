/**
 * @file task_manager.h
 * @brief 异步任务管理器
 * @details 管理任务执行、进度跟踪和协作式取消
 * @version 1.0.0
 */

#pragma once

#include <functional>
#include <string>
#include <memory>
#include <vector>
#include <atomic>
#include <mutex>
#include <chrono>

namespace agent {

enum class TaskType {
    Normal,
    Background,
    Blocking,
    Critical
};

enum class TaskStatus {
    Pending,
    Running,
    Completed,
    Cancelled,
    Failed
};

class TaskManager;

class Task : public std::enable_shared_from_this<Task> {
public:
    using TaskFunc = std::function<void(const std::atomic<bool>& should_cancel)>;

    Task(std::string name, TaskFunc func, float max_progress = 100.0f);
    ~Task();

    [[nodiscard]] const std::string& getName() const { return m_name; }
    [[nodiscard]] TaskType getType() const { return m_type; }
    void setType(const TaskType& type) { m_type = type; }
    [[nodiscard]] TaskStatus getStatus() const { return m_status; }

    [[nodiscard]] float getProgress() const { return m_progress; }
    [[nodiscard]] float getMaxProgress() const { return m_max_progress; }
    [[nodiscard]] float getProgressPercent() const {
        return m_max_progress > 0 ? m_progress / m_max_progress : 0;
    }

    void setProgress(float progress) {
        m_progress = std::min(progress, m_max_progress);
        if (m_progress >= m_max_progress) m_status = TaskStatus::Completed;
    }

    void addProgress(float delta) {
        m_progress += delta;
        if (m_progress >= m_max_progress) {
            m_progress = m_max_progress;
            m_status = TaskStatus::Completed;
        }
    }

    void cancel() {
        m_should_cancel = true;
        m_status = TaskStatus::Cancelled;
    }

    [[nodiscard]] bool shouldCancel() const { return m_should_cancel; }

    [[nodiscard]] bool isFinished() const {
        return m_status == TaskStatus::Completed ||
               m_status == TaskStatus::Cancelled ||
               m_status == TaskStatus::Failed;
    }

    [[nodiscard]] bool isRunning() const {
        return m_status == TaskStatus::Running;
    }

    void onCompleted(std::function<void()> callback) {
        m_completed_callback = std::move(callback);
    }

private:
    void execute();
    void markCompleted();
    void markFailed(const std::string& error_message = "Unknown error");

private:
    std::string m_name;
    TaskFunc m_func;
    TaskType m_type = TaskType::Normal;
    TaskStatus m_status = TaskStatus::Pending;

    float m_progress = 0.0F;
    float m_max_progress;

    std::atomic<bool> m_should_cancel{false};
    std::function<void()> m_completed_callback;

    std::chrono::steady_clock::time_point m_start_time;

    friend class TaskManager;
};

class TaskManager final {
public:
    static TaskManager& instance() noexcept {
        static TaskManager inst;
        return inst;
    }

    TaskManager(const TaskManager&) = delete;
    TaskManager& operator=(const TaskManager&) = delete;
    TaskManager(TaskManager&&) = delete;
    TaskManager& operator=(TaskManager&&) = delete;

    std::shared_ptr<Task> create(
        const std::string& name,
        Task::TaskFunc func,
        TaskType type = TaskType::Normal
    );

    std::shared_ptr<Task> launch(
        const std::string& name,
        Task::TaskFunc func,
        TaskType type = TaskType::Normal
    );

    void start(std::shared_ptr<Task> task);
    void cancel(std::shared_ptr<Task> task);

    [[nodiscard]] const std::vector<std::shared_ptr<Task>>& getTasks() const {
        return m_tasks;
    }

    [[nodiscard]] std::vector<std::shared_ptr<Task>> getRunningTasks() const;
    void update();
    void waitForAll();
    void cancelAll();
    [[nodiscard]] size_t getRunningTaskCount() const;

private:
    TaskManager() = default;
    ~TaskManager();

    std::vector<std::shared_ptr<Task>> m_tasks;
    mutable std::mutex m_tasks_mutex;
};

} // namespace workx
