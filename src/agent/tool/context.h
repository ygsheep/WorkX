/**
 * @file context.h
 * @brief ToolContext — 工具执行上下文
 * @details 在工具执行过程中传递的运行时信息：会话 ID、工作目录、权限模式、取消信号、
 *          任务管理器、进度回调
 * @version 1.2.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <atomic>
#include <functional>
#include <nlohmann/json.hpp>

namespace agent {

// D-5：前向声明，避免 context.h 强依赖 i_config_manager.h
class IConfigManager;

// 前向声明：ITaskManager 在 agent 命名空间下（非 agent::task）
class ITaskManager;

// 前向声明：IEventBus（用于 AskUserTool 等需要发布事件的工具）
class IEventBus;

namespace tool {

/// @brief 工具进度回调类型
/// @details 工具执行过程中上报进度文本（如 stdout 增量、心跳信息），
///          由 ReActLoop 注入，最终通过 EventBus 发布到 UI。
/// @param progress_text 进度文本
using ProgressCallback = std::function<void(const std::string& progress_text)>;

/// @brief 权限模式（#36 统一权限决策层）
/// @details 工具 check_permissions 的决策依据，由宿主在构造 ToolContext 时注入：
///          - Default：常规操作放行，危险操作（敏感路径/目录逃逸/敏感命令）AskUser 确认
///          - AcceptEdits：接受编辑类自动放行（CC 兼容占位）
///          - Plan：计划/只读模式，禁止写文件与执行命令
///          - BypassPermissions：完全放行（用户显式授权后）
enum class PermissionMode : uint8_t {
    Default = 0,
    AcceptEdits = 1,
    Plan = 2,
    BypassPermissions = 3,
};

    /// @brief 工具 touch 回调类型
    /// @details 工具执行过程中上报访问过的文件路径（绝对路径），
    ///          由 ReActLoop 注入，用于 conditional skills 的路径匹配。
    /// @param path 访问的文件路径（绝对路径）
    using TouchCallback = std::function<void(const std::string& path)>;

    /// @brief 文件系统变更回调类型
    /// @details 工具写入/删除文件后调用（无参数），由宿主注入，
    ///          用于通知宿主失效文件索引（如 TUI @ 补全索引）等缓存。
    using FileSystemChangedCallback = std::function<void()>;

/// @brief 工具执行上下文
///
/// 在工具执行过程中传递的运行时信息：
/// - 当前会话 ID 与请求 ID
/// - 工作目录路径
/// - 权限模式
/// - 中断信号（CancellationToken）
/// - 任务管理器（用于后台任务）
/// - 进度回调
struct ToolContext {
    std::string cwd;                        ///< 工作目录
    std::string session_id;                 ///< 会话 ID
    std::string request_id;                 ///< 请求 ID
    std::string model;                      ///< 当前模型名称
    nlohmann::json options;                 ///< 额外选项

    /// @brief 权限模式（#36 权限决策层）
    /// @details 默认 Default；宿主可在构造时注入（如 CLI 的 --bypass-permissions）。
    ///          工具 check_permissions 依据该模式决定放行/确认/拒绝。
    PermissionMode permission_mode{PermissionMode::Default};

    /// @brief 外部取消信号指针（可选）
    /// @details 2.3 修复：由调用方（ReActLoop）传入 should_cancel 的地址，
    ///          使工具能即时感知外部取消请求。nullptr 时回退到内部 cancelled_。
    ///          生命周期由调用方保证（栈变量通常在 ToolContext 之上存活）。
    const std::atomic<bool>* cancel_flag = nullptr;

    /// @brief H-5：配置管理器指针（必填，非拥有）
    /// @details 由调用方（ReActLoop）显式注入，工具通过 config_manager() 访问。
    ///          H-5 移除单例回退：nullptr 时 config_manager() 抛 std::logic_error，
    ///          强制 DI 显式依赖。生命周期由调用方保证（通常为 ChatSession 的成员）。
    IConfigManager* config_manager_ptr = nullptr;

    /// @brief 任务管理器指针（可选，非拥有）
    /// @details 由调用方（ReActLoop）显式注入，工具通过 task_manager() 访问。
    ///          nullptr 时 task_manager() 抛 std::logic_error。
    ///          用于 BashTool 等需要启动后台任务的工具。
    ///          生命周期由调用方保证（通常为 ChatSession 引用的 TaskManager 单例）。
    ITaskManager* task_manager_ptr = nullptr;

    /// @brief 事件总线指针（可选，非拥有）
    /// @details 由调用方（ReActLoop）显式注入，工具通过 event_bus() 访问。
    ///          nullptr 时 event_bus() 抛 std::logic_error。
    ///          用于 AskUserTool 等需要发布事件的工具。
    ///          生命周期由调用方保证（通常为 ChatSession 持有的 EventBus 引用）。
    IEventBus* event_bus_ptr = nullptr;

    /// @brief 进度回调（可选）
    /// @details 由调用方（ReActLoop）注入，工具在长任务执行过程中调用以上报进度。
    ///          默认为空（无进度上报）。生命周期由 ToolContext 所有者保证。
    ProgressCallback progress_callback = nullptr;

    /// @brief touch 回调（可选）
    /// @details 由调用方（ReActLoop）注入，工具访问文件时调用以上报路径，
    ///          供 conditional skills 匹配激活。默认为空。生命周期由 ToolContext 所有者保证。
    TouchCallback touch_callback = nullptr;

    /// @brief 文件系统变更回调（可选）
    /// @details 由调用方（ReActLoop）注入，工具写入/删除文件后调用，
    ///          宿主据此失效文件索引等缓存。默认为空（无宿主，no-op）。
    ///          生命周期由 ToolContext 所有者保证。
    FileSystemChangedCallback on_file_system_changed = nullptr;

    /// @brief 解析配置管理器（H-5：nullptr 时抛异常，不再回退单例）
    /// @return IConfigManager 引用
    /// @throws std::logic_error 当 config_manager_ptr == nullptr
    IConfigManager& config_manager() const;

    /// @brief 解析任务管理器
    /// @return ITaskManager 引用
    /// @throws std::logic_error 当 task_manager_ptr == nullptr
    ITaskManager& task_manager() const;

    /// @brief 解析事件总线
    /// @return IEventBus 引用
    /// @throws std::logic_error 当 event_bus_ptr == nullptr
    IEventBus& event_bus() const;

    /// @brief 检查是否已取消
    /// @return 已取消返回 true
    bool is_cancelled() const {
        if (cancel_flag != nullptr) {
            return cancel_flag->load(std::memory_order_acquire);
        }
        return cancelled_.load(std::memory_order_relaxed);
    }

    /// @brief 请求取消（仅当未绑定外部 cancel_flag 时有效）
    void cancel() {
        if (cancel_flag == nullptr) {
            cancelled_.store(true, std::memory_order_relaxed);
        }
        // 绑定外部 cancel_flag 时由外部负责置位，本方法无操作
    }

    /// @brief 上报进度（若 progress_callback 已设置）
    /// @param progress_text 进度文本
    void report_progress(const std::string& progress_text) const {
        if (progress_callback) {
            progress_callback(progress_text);
        }
    }

    /// @brief 上报 touch（若 touch_callback 已设置）
    /// @param path 访问的文件路径（绝对路径）
    void report_touch(const std::string& path) const {
        if (touch_callback) {
            touch_callback(path);
        }
    }

private:
    std::atomic<bool> cancelled_{false};    ///< 内部取消标志（fallback）
};

} // namespace agent::tool
} // namespace agent
