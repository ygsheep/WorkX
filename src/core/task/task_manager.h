/**
 * @file task_manager.h
 * @brief 异步任务管理器
 * @details 管理任务执行、进度跟踪和协作式取消
 *          - 使用 ThreadPool 替代裸 std::thread（2.2）
 *          - 抽取 ITaskManager 接口支持 DI（D-1）
 * @version 2.0.0
 */

#pragma once

#include <functional>
#include <string>
#include <memory>
#include <vector>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <thread>
#include <type_traits>

#include "core/task/thread_pool.h"
#include "core/events/event_bus.h"

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
    using FinishedCallback = std::function<void()>;

    // T-6：编译期验证 start_time 原子字段满足 trivially copyable 要求
    // 注意：MSVC 对 std::atomic<T> 的 is_trivially_copyable 实现有历史 bug，
    //       改为验证底层类型 int64_t 满足 trivially copyable（标准保证 atomic<T> 与 T 同为 trivially copyable）
    static_assert(std::is_trivially_copyable_v<int64_t>,
                  "int64_t must be trivially copyable (atomic<int64_t> shares this property)");

    /// @brief 构造
    /// @param event_bus 事件总线引用（D-1 DI：Task 通过它发布生命周期事件）
    /// @param on_finished 任务结束时的通知回调（用于唤醒 TaskManager::waitForAll）
    Task(std::string name, TaskFunc func,
         IEventBus& event_bus,
         FinishedCallback on_finished = {},
         float max_progress = 100.0f);
    ~Task();

    [[nodiscard]] const std::string& getName() const { return m_name; }
    [[nodiscard]] TaskType getType() const { return m_type; }
    void setType(const TaskType& type) { m_type = type; }
    /// @brief 获取任务状态（线程安全，原子读取）
    [[nodiscard]] TaskStatus getStatus() const {
        return m_status.load(std::memory_order_acquire);
    }

    /// @brief 获取当前进度（线程安全，原子读取）
    [[nodiscard]] float getProgress() const {
        return m_progress.load(std::memory_order_relaxed);
    }
    /// @brief 获取进度上限（线程安全，原子读取）
    [[nodiscard]] float getMaxProgress() const {
        return m_max_progress.load(std::memory_order_relaxed);
    }
    [[nodiscard]] float getProgressPercent() const {
        const float max = m_max_progress.load(std::memory_order_relaxed);
        if (max <= 0) return 0;
        return m_progress.load(std::memory_order_relaxed) / max;
    }

    /// @brief 设置进度（线程安全）。达到上限时自动标记 Completed
    void setProgress(float progress) {
        const float max = m_max_progress.load(std::memory_order_relaxed);
        const float clamped = std::min(progress, max);
        m_progress.store(clamped, std::memory_order_relaxed);
        if (clamped >= max) {
            m_status.store(TaskStatus::Completed, std::memory_order_release);
        }
    }

    /// @brief 增量更新进度（线程安全）
    void addProgress(float delta) {
        const float max = m_max_progress.load(std::memory_order_relaxed);
        // CAS 循环：保证并发 addProgress 不丢失更新
        float current = m_progress.load(std::memory_order_relaxed);
        float next;
        do {
            next = std::min(current + delta, max);
        } while (!m_progress.compare_exchange_weak(current, next,
            std::memory_order_relaxed, std::memory_order_relaxed));
        if (next >= max) {
            m_status.store(TaskStatus::Completed, std::memory_order_release);
        }
    }

    // 仅设置取消请求标志，不立即修改 status；由 execute() 检测后置 Cancelled
    void cancel() {
        m_should_cancel.store(true, std::memory_order_release);
    }

    [[nodiscard]] bool shouldCancel() const {
        return m_should_cancel.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool isFinished() const {
        const auto s = m_status.load(std::memory_order_acquire);
        return s == TaskStatus::Completed ||
               s == TaskStatus::Cancelled ||
               s == TaskStatus::Failed;
    }

    [[nodiscard]] bool isRunning() const {
        return m_status.load(std::memory_order_acquire) == TaskStatus::Running;
    }

    void onCompleted(std::function<void()> callback) {
        m_completed_callback = std::move(callback);
    }

private:
    void execute();
    void markCompleted();
    void markFailed(const std::string& error_message = "Unknown error");

    /// @brief 读取开始时间点（线程安全）
    [[nodiscard]] std::chrono::steady_clock::time_point start_time() const noexcept {
        return std::chrono::steady_clock::time_point{
            std::chrono::nanoseconds{m_start_time_ns.load(std::memory_order_relaxed)}
        };
    }

private:
    std::string m_name;
    TaskFunc m_func;
    TaskType m_type = TaskType::Normal;
    // 2.4 / L-4: 状态与进度字段全部原子化，消除 setProgress/execute 间的数据竞争
    std::atomic<TaskStatus> m_status{TaskStatus::Pending};
    std::atomic<float> m_progress{0.0F};
    std::atomic<float> m_max_progress;

    std::atomic<bool> m_should_cancel{false};
    std::function<void()> m_completed_callback;

    // T-6：start_time 原子化（存 nanoseconds since epoch），消除潜在数据竞争
    // 使用 int64_t 而非 time_point，确保跨平台 trivially copyable
    std::atomic<int64_t> m_start_time_ns{0};

    // D-1：DI 注入的事件总线与结束回调
    IEventBus& m_event_bus;
    FinishedCallback m_on_finished;

    friend class TaskManager;
};

// ============================================================
// ITaskManager 接口（D-1 DI 化）
// ============================================================

/// @brief 任务管理器抽象接口
/// @details 允许测试注入 MockTaskManager，解除对单例的硬依赖。
///          生产代码用 TaskManager（继承 ITaskManager）。
class ITaskManager {
public:
    virtual ~ITaskManager() = default;

    virtual std::shared_ptr<Task> create(
        const std::string& name,
        Task::TaskFunc func,
        TaskType type = TaskType::Normal) = 0;

    virtual std::shared_ptr<Task> launch(
        const std::string& name,
        Task::TaskFunc func,
        TaskType type = TaskType::Normal) = 0;

    virtual void start(std::shared_ptr<Task> task) = 0;
    virtual void cancel(std::shared_ptr<Task> task) = 0;

    [[nodiscard]] virtual std::vector<std::shared_ptr<Task>> getTasks() const = 0;
    [[nodiscard]] virtual std::vector<std::shared_ptr<Task>> getRunningTasks() const = 0;
    [[nodiscard]] virtual size_t getRunningTaskCount() const = 0;

    virtual void update() = 0;
    virtual void waitForAll() = 0;
    virtual void cancelAll() = 0;
};

// ============================================================
// TaskManager 默认实现（基于 ThreadPool）
// ============================================================

class TaskManager final : public ITaskManager {
public:
    /// @brief 单例访问（向后兼容，新代码应优先 DI 注入）
    static TaskManager& instance() noexcept {
        static TaskManager inst;
        return inst;
    }

    TaskManager(const TaskManager&) = delete;
    TaskManager& operator=(const TaskManager&) = delete;
    TaskManager(TaskManager&&) = delete;
    TaskManager& operator=(TaskManager&&) = delete;

    /// @brief 构造（D-1 DI：可注入 IEventBus，默认使用全局单例）
    explicit TaskManager(IEventBus& event_bus = EventBus::instance())
        : m_pool(0), m_event_bus(event_bus) {}

    std::shared_ptr<Task> create(
        const std::string& name,
        Task::TaskFunc func,
        TaskType type = TaskType::Normal
    ) override;

    std::shared_ptr<Task> launch(
        const std::string& name,
        Task::TaskFunc func,
        TaskType type = TaskType::Normal
    ) override;

    void start(std::shared_ptr<Task> task) override;
    void cancel(std::shared_ptr<Task> task) override;

    [[nodiscard]] std::vector<std::shared_ptr<Task>> getTasks() const override;
    [[nodiscard]] std::vector<std::shared_ptr<Task>> getRunningTasks() const override;
    [[nodiscard]] size_t getRunningTaskCount() const override;

    void update() override;
    void waitForAll() override;
    void cancelAll() override;

    /// @brief 工作线程数（诊断 / 测试用）
    [[nodiscard]] size_t worker_count() const noexcept { return m_pool.worker_count(); }
    /// @brief 队列积压数（诊断用）
    [[nodiscard]] size_t pending_count() const { return m_pool.pending_count(); }

private:
    ~TaskManager() override;

    std::vector<std::shared_ptr<Task>> m_entries;
    mutable std::mutex m_tasks_mutex;
    std::condition_variable m_tasks_cv;
    ThreadPool m_pool;
    IEventBus& m_event_bus;

    friend class Task;  // Task::execute 结束时通知 m_tasks_cv
};

} // namespace agent
