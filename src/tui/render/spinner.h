/**
 * @file spinner.h
 * @brief 思考计时动画
 * @details 替代原 |/-\ 旋转，驱动 StatusBar 的思考帧更新和秒数计时
 * @version 2.0.0
 */

#pragma once

#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <functional>

namespace workx {

class Terminal;
class StatusBar;

/**
 * @brief 思考计时动画
 * @details 驱动 StatusBar 的思考指示器更新（帧动画 + 秒数递增）
 *          不再自己写旋转字符到终端，而是通过回调通知 StatusBar 更新
 */
class Spinner {
public:
    /// @brief 思考动画更新回调
    using UpdateCallback = std::function<void(int32_t elapsed_seconds)>;

    explicit Spinner(Terminal* terminal);
    ~Spinner();

    /// @brief 启动思考计时
    /// @param msg 消息（保留接口兼容，实际不再显示）
    void start(std::string_view msg);

    /// @brief 停止思考计时
    void stop();

    /// @brief 设置思考动画更新回调（StatusBar 使用）
    void set_update_callback(UpdateCallback cb);

    /// @brief 是否正在运行
    bool is_running() const { return m_running; }

    /// @brief 获取已思考秒数
    int32_t elapsed_seconds() const;

private:
    void run();

    Terminal* m_terminal;
    std::atomic<bool> m_running{false};
    std::string m_message;
    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::chrono::steady_clock::time_point m_start_time;
    UpdateCallback m_update_callback;

    static constexpr int FRAME_INTERVAL_MS = 100;  // 100ms（10帧布莱叶旋转）
};

} // namespace workx
