/**
 * @file task_manager.h
 * @brief 异步任务管理器
 * @details 管理任务执行、进度跟踪和协作式取消
 * @version 1.0.0
 *
 * 使用方式：复制 task_manager.h + task_manager.cpp 到项目
 * 可选：复制 task_events.h 并启用 EventBus 集成
 */

#pragma once

#include <functional>
#include <string>
#include <memory>
#include <vector>
#include <atomic>
#include <mutex>
#include <chrono>

// ============================================================
// 命名空间：按需修改
// ============================================================
namespace mydev {

/**
 * @brief 任务类型
 */
enum class TaskType {
    Normal,     ///< 普通任务
    Background, ///< 后台任务（不影响UI）
    Blocking,   ///< 阻塞任务（当前线程执行）
    Critical    ///< 关键任务（高优先级）
};

/**
 * @brief 任务状态
 */
enum class TaskStatus {
    Pending,    ///< 等待执行
    Running,    ///< 正在执行
    Completed,  ///< 已完成
    Cancelled,  ///< 已取消
    Failed      ///< 失败
};

class TaskManager;

/**
 * @brief 任务类
 */
class Task : public std::enable_shared_from_this<Task> {
public:
    using TaskFunc = std::function<void(const std::atomic<bool>& should_cancel)>;

    /**
     * @brief 构造函数
     * @param name 任务名称
     * @param func 任务函数
     * @param max_progress 最大进度值（默认 100）
     */
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

    /** @brief 更新进度（达 max_progress 自动标记 Completed） */
    void setProgress(float progress) {
        m_progress = std::min(progress, m_max_progress);
        if (m_progress >= m_max_progress) m_status = TaskStatus::Completed;
    }

    /** @brief 增量进度 */
    void addProgress(float delta) {
        m_progress += delta;
        if (m_progress >= m_max_progress) {
            m_progress = m_max_progress;
            m_status = TaskStatus::Completed;
        }
    }

    /** @brief 取消任务（协作式，设置原子标志） */
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

    /** @brief 设置完成回调 */
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

/**
 * @brief 任务管理器（单例）
 */
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

    /** @brief 创建任务（不启动） */
    std::shared_ptr<Task> create(
        const std::string& name,
        Task::TaskFunc func,
        TaskType type = TaskType::Normal
    );

    /** @brief 创建并立即启动 */
    std::shared_ptr<Task> launch(
        const std::string& name,
        Task::TaskFunc func,
        TaskType type = TaskType::Normal
    );

    /** @brief 启动已创建的任务 */
    void start(std::shared_ptr<Task> task);

    /** @brief 取消任务 */
    void cancel(std::shared_ptr<Task> task);

    [[nodiscard]] const std::vector<std::shared_ptr<Task>>& getTasks() const {
        return m_tasks;
    }

    [[nodiscard]] std::vector<std::shared_ptr<Task>> getRunningTasks() const;

    /** @brief 主循环中调用，清理已完成任务 */
    void update();

    /** @brief 等待所有任务完成 */
    void waitForAll();

    /** @brief 取消所有任务 */
    void cancelAll();

    [[nodiscard]] size_t getRunningTaskCount() const;

private:
    TaskManager() = default;
    ~TaskManager();

    std::vector<std::shared_ptr<Task>> m_tasks;
    mutable std::mutex m_tasks_mutex;
};

} // namespace mydev
