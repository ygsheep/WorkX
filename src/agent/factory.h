/**
 * @file factory.h
 * @brief Agent 层会话装配工厂（宿主无关，B1 复用装配）
 * @details 从 workx_app/factory.cpp 上提的宿主无关装配逻辑：
 *          - create_session: Backend + ChatSession 组装
 *          - register_builtin_tools: 内置工具注册（单一来源，工具集同步）
 *          - build_system_prompt: 系统提示词拼接（env + 项目记忆 + 工具 prompt）
 *          依赖仅限 agent + core，不触碰 tui/app，供多个宿主（workx / codex）复用。
 * @version 1.0.0
 */

#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "core/config/i_config_manager.h"

namespace agent { namespace tool { class ToolRegistry; } }
namespace agent::session { class SessionStore; }

namespace agent {

class ChatSession;
class IBackendAdmin;
struct ProviderPreset;
class ITaskManager;
class IEventBus;

/// @brief 会话创建结果（工厂返回）
/// @details H-8：新增 backend_admin 字段，UI 层通过它调用 list_models /
///          set_model_name 等管理接口，避免暴露完整 IBackend*。
struct SessionResult {
    std::unique_ptr<ChatSession> session;  ///< 创建的会话（无 remote_url 时为 nullptr）
    std::string remote_url;                ///< 解析后的 API URL
    std::string model_name;                ///< 解析后的模型名
    IBackendAdmin* backend_admin = nullptr;  ///< H-8：后端管理句柄（非拥有，session 持有 backend 生命周期）
};

/// @brief 创建 Backend + ChatSession
/// @details 执行流程：
///          1. 从 cfg 读取 provider，查找 ProviderPreset
///          2. 解析 remote_url（cfg > preset > ""）和 model_name（cfg > preset > ""）
///          3. 若 remote_url 非空：创建 Backend → initialize → 构造 ChatSession
///          4. 注册内置工具、拼接系统提示词
///          5. H-8：返回 IBackendAdmin* 给 UI 层调用管理接口
/// @param cfg 配置管理器
/// @param preset Provider 预设（nullptr 表示无预设）
/// @param task_manager 任务管理器（M-1：显式 DI，替代 TaskManager::instance()）
/// @param event_bus 事件总线（M-1：显式 DI，替代 EventBus::instance()）
/// @return SessionResult（session 可能为 nullptr）
SessionResult create_session(IConfigManager& cfg,
                             const ProviderPreset* preset,
                             ITaskManager& task_manager,
                             IEventBus& event_bus);

/// @brief 注册内置工具到 ToolRegistry（全量工具集，单一来源）
/// @details 注册 FileRead/FileWrite/FileEdit/SkillTool/Bash/Glob/Grep/AskUser/
///          PlanMode/AgentTool/TaskOutput/TaskStop；Windows 额外含 PowerShellTool。
///          新增工具只需在此处维护，各宿主（workx / codex）自动同步。
void register_builtin_tools(tool::ToolRegistry& registry);

/// @brief 拼接系统提示词（含环境上下文、项目记忆和工具 prompt）
/// @param user_prompt 用户配置的系统提示词（可为空）
/// @param registry 已注册工具的工具注册表
/// @return 拼接后的完整系统提示词
std::string build_system_prompt(const std::string& user_prompt,
                                const tool::ToolRegistry& registry);

} // namespace agent
