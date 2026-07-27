/**
 * @file factory.h
 * @brief 应用层依赖组装工厂（D-2）
 * @details 从 main.cpp 提取的可测试组装逻辑：
 *          - init_logger: 日志系统初始化
 *          - make_terminal_config: 终端配置构建
 *          - create_session: Backend + ChatSession 组装
 *          - register_builtin_tools: 内置工具注册
 *          - build_system_prompt: 系统提示词拼接
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "core/config/i_config_manager.h"

namespace tui { struct TerminalConfig; }
namespace agent { namespace tool { class ToolRegistry; } }

namespace agent {

class ChatSession;
struct ProviderPreset;
class ITaskManager;
class IEventBus;

/// @brief 会话创建结果（D-2 工厂返回）
struct SessionResult {
    std::unique_ptr<ChatSession> session;  ///< 创建的会话（无 remote_url 时为 nullptr）
    std::string remote_url;                ///< 解析后的 API URL
    std::string model_name;                ///< 解析后的模型名
};

/// @brief 初始化日志系统
/// @details 从配置读取 level 和 file 路径，配置 Logger
/// @param cfg 配置管理器
/// @param default_log_path 默认日志文件路径（cfg 中 LOG_FILE 为空时使用）
void init_logger(IConfigManager& cfg, const std::filesystem::path& default_log_path);

/// @brief 从配置构建 TerminalConfig
/// @param cfg 配置管理器
/// @return 填充好的 TerminalConfig
tui::TerminalConfig make_terminal_config(IConfigManager& cfg);

/// @brief 创建 Backend + ChatSession
/// @details 执行流程：
///          1. 从 cfg 读取 provider，查找 ProviderPreset
///          2. 解析 remote_url（cfg > preset > ""）和 model_name（cfg > preset > ""）
///          3. 若 remote_url 非空：创建 Backend → initialize → 构造 ChatSession
///          4. 注册内置工具、拼接系统提示词
/// @param cfg 配置管理器
/// @param preset Provider 预设（nullptr 表示无预设）
/// @param task_manager 任务管理器（M-1：显式 DI，替代 TaskManager::instance()）
/// @param event_bus 事件总线（M-1：显式 DI，替代 EventBus::instance()）
/// @return SessionResult（session 可能为 nullptr）
SessionResult create_session(IConfigManager& cfg,
                             const ProviderPreset* preset,
                             ITaskManager& task_manager,
                             IEventBus& event_bus);

/// @brief 注册内置工具到 ToolRegistry
/// @details 注册 FileReadTool / FileWriteTool / FileEditTool
void register_builtin_tools(tool::ToolRegistry& registry);

/// @brief 拼接系统提示词（含工具 prompt 和 @file 引用说明）
/// @param user_prompt 用户配置的系统提示词（可为空）
/// @param registry 已注册工具的工具注册表
/// @return 拼接后的完整系统提示词
std::string build_system_prompt(const std::string& user_prompt,
                                const tool::ToolRegistry& registry);

} // namespace agent
