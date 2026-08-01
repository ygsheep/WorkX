/**
 * @file factory.cpp
 * @brief 应用层依赖组装工厂实现（D-2）
 * @details 从 main.cpp 提取的可测试组装逻辑
 * @version 1.0.0
 * @date 2026-07
 */

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX  // 防止 windows.h 定义 min/max 宏干扰 std::numeric_limits
#endif
#include <windows.h>
#else
#include <sys/utsname.h>
#endif

#include <liblogger/logger.h>

#include "app/config/app_config.h"
#include "app/factory.h"
#include "agent/api/backend_factory.h"
#include "agent/api/chat_types.h"
#include "agent/api/i_backend.h"
#include "agent/api/i_backend_admin.h"  // C-2：dynamic_cast 到 IBackendAdmin*
#include "agent/core/chat_session.h"
#include "agent/model/provider_preset.h"
#include "agent/prompt/memory.h"  // 项目记忆加载（CLAUDE.md / AGENT.md）
#include "agent/session/session_store.h"  // 项目会话恢复
#include "agent/tool/BashTool/bash_tool.h"
#include "agent/tool/AskUser/AskUserTool.h"
#include "agent/tool/FileEditTool/file_edit_tool.h"
#include "agent/tool/FileReadTool/file_read_tool.h"
#include "agent/tool/FileWriteTool/file_write_tool.h"
#include "agent/tool/GlobTool/glob_tool.h"
#include "agent/tool/GrepTool/grep_tool.h"
#include "agent/tool/PowerShellTool/powershell_tool.h"
#include "agent/tool/ShellTool/shell_detector.h"
#include "agent/tool/registry.h"
#include "core/config/config_manager.h"
#include "core/events/event_bus.h"
#include "core/events/i_event_bus.h"
#include "core/task/task_manager.h"
#include "core/utils/uuid.h"  // 项目会话恢复：UUID 生成
#include "tui/core/terminal.h"

namespace agent {

// ============================================================
// init_logger
// ============================================================

void init_logger(IConfigManager& cfg, const std::filesystem::path& default_log_path) {
    auto& logger = agent::log::Logger::get_instance();

    // 日志级别
    std::string level_str = cfg.get_or<std::string>(keys::LOG_LEVEL, "info");
    std::transform(level_str.begin(), level_str.end(), level_str.begin(), ::tolower);
    if (level_str == "trace")       logger.set_level(agent::log::LogLevel::TRACE);
    else if (level_str == "debug")  logger.set_level(agent::log::LogLevel::DEBUG);
    else if (level_str == "warn")   logger.set_level(agent::log::LogLevel::WARN);
    else if (level_str == "error")  logger.set_level(agent::log::LogLevel::ERROR);
    else if (level_str == "fatal")  logger.set_level(agent::log::LogLevel::FATAL);
    else                            logger.set_level(agent::log::LogLevel::INFO);

    // 日志文件
    std::string log_file = cfg.get_or<std::string>(keys::LOG_FILE, "");
    if (log_file.empty()) {
        log_file = default_log_path.string();
    }
    logger.enable_file_output(log_file, true);
}

// ============================================================
// make_terminal_config
// ============================================================

tui::TerminalConfig make_terminal_config(IConfigManager& cfg) {
    tui::TerminalConfig config;
    config.simple_io = cfg.get_or<bool>(keys::SIMPLE_IO, false);
    config.use_color = !cfg.get_or<bool>(keys::NO_COLOR, false);
    config.prompt_string = cfg.get_or<std::string>(keys::PROMPT, "> ");
    return config;
}

// ============================================================
// create_session
// ============================================================

SessionResult create_session(IConfigManager& cfg,
                             const ProviderPreset* preset,
                             ITaskManager& task_manager,
                             IEventBus& event_bus) {
    SessionResult result;

    // URL: cfg(显式设置) > preset > ""
    if (cfg.has(keys::REMOTE_URL)) {
        result.remote_url = cfg.get_or<std::string>(keys::REMOTE_URL, "");
    } else if (preset && !preset->default_url.empty()) {
        result.remote_url = std::string(preset->default_url);
    }

    // Model: cfg(显式设置) > preset > ""
    if (cfg.has(keys::MODEL_NAME)) {
        result.model_name = cfg.get_or<std::string>(keys::MODEL_NAME, "");
    } else if (preset && !preset->default_model.empty()) {
        result.model_name = std::string(preset->default_model);
    }

    // 无 remote_url 时不创建 backend
    if (result.remote_url.empty()) {
        return result;
    }

    // 构建 BackendConfig
    BackendConfig backend_config;
    backend_config.type = BackendConfig::Type::Remote;
    backend_config.provider = preset ? preset->type : ProviderType::OpenAI;
    backend_config.base_url = result.remote_url;
    backend_config.model_name = result.model_name;
    backend_config.api_key = cfg.get_or<std::string>(keys::API_KEY, "");
    int default_timeout = preset && preset->timeout_ms > 0 ? preset->timeout_ms : 30000;
    backend_config.timeout_ms = cfg.get_or<int>(keys::TIMEOUT_MS, default_timeout);
    // DS_CACHE P2：reasoning_content 往返配置（默认 false，仅 DeepSeek-reasoner 等 thinking 模型开启）
    backend_config.send_reasoning_content = cfg.get_or<bool>(keys::SEND_REASONING, false);

    // 创建后端（H-1：显式注入 event_bus 以保留 BackendStatusEvent 发布；
    //              M-1：不再回退 EventBus::instance()）
    auto backend = BackendFactory::create(backend_config, &event_bus);
    if (!backend) {
        return result;  // session 保持 nullptr
    }

    // 初始化后端（V2-3：initialize 返回 ResultV2）
    auto init_result = backend->initialize(backend_config);
    if (init_result.is_err()) {
        return result;  // session 保持 nullptr
    }

    // 构造 ChatSession（M-1：显式注入 task_manager / event_bus / cfg，不再用单例）
    // C-2：先构造 session，再从 session 暴露的 admin 接口获取 backend_admin
    //      （避免 std::move(backend) 之前赋值导致 ChatSession 构造抛异常时悬垂指针）
    // 项目会话恢复：生成 UUID 作为 session_id（替换硬编码 "default"）
    std::string session_id = core::util::generate_uuid();
    int default_retry_delay = preset && preset->retry_delay_ms > 0 ? preset->retry_delay_ms : 1000;
    result.session = std::make_unique<ChatSession>(
        std::move(backend),
        task_manager,
        event_bus,
        cfg,
        default_retry_delay, session_id);

    // C-2：session 构造成功后，backend 已由 session 持有。
    // 通过 ChatSession 暴露的 completion_provider() 获取 ICompletionProvider*，
    // 再 dynamic_cast 到 IBackendAdmin*（IBackend 同时继承两者）。
    // session 存活期间 backend_admin 始终有效；session 析构后禁止使用。
    if (auto* provider = result.session->completion_provider()) {
        result.backend_admin = dynamic_cast<IBackendAdmin*>(provider);
    }

    // 注册内置工具
    auto tool_registry = std::make_shared<tool::ToolRegistry>();
    register_builtin_tools(*tool_registry);
    result.session->set_tool_registry(tool_registry);

    // 系统提示词
    std::string sys_prompt = build_system_prompt(
        cfg.get_or<std::string>(keys::SYSTEM_PROMPT, ""), *tool_registry);
    if (!sys_prompt.empty()) {
        result.session->set_system_prompt(sys_prompt);
    }

    // DS_CACHE H-4：从 provider preset 或 cfg 注入上下文窗口到压缩器
    // 优先级：cfg.backend.context_length > preset.default_context_length > 0（压缩器内部 fallback 1M）
    int32_t context_window = cfg.get_or<int>(keys::CONTEXT_LENGTH, 0);
    if (context_window <= 0 && preset && preset->default_context_length > 0) {
        context_window = preset->default_context_length;
    }
    if (context_window > 0) {
        result.session->set_compactor_context_window(context_window);
    }

    // DS_CACHE M-1：配置归档目录（compact 折叠前归档原消息，保证可追溯）
    // 派生自 session.save_path 的父目录 / "archive"，未配置 save_path 则跳过
    std::string save_path = cfg.get_or<std::string>(keys::SAVE_PATH, "");
    if (!save_path.empty()) {
        namespace fs = std::filesystem;
        fs::path archive_dir = fs::path(save_path).parent_path() / "archive";
        result.session->set_compactor_archive_dir(archive_dir.string());
    }

    // ============================================================
    // 项目会话恢复：配置懒创建 SessionStore（首条 user 消息时才创建文件）
    // ============================================================
    // 存储路径：<config_dir>/projects/<编码路径>/<session_id>.jsonl
    // factory 只传配置，不创建文件；ChatSession 在首条 user 消息时懒创建
    try {
        namespace fs = std::filesystem;
        fs::path config_dir = default_config_path().parent_path();
        std::string cwd = fs::current_path().string();
        fs::path project_dir = session::get_project_session_dir(config_dir, cwd);

        std::string git_branch;
        if (fs::exists(fs::current_path() / ".git")) {
            git_branch = "unknown";
        }
        result.session->configure_session_store(
            project_dir.string(), cwd, result.model_name, git_branch);
    } catch (const std::exception&) {
        // 配置失败不阻断会话启动，仅失去持久化能力
    }

    return result;
}

// ============================================================
// register_builtin_tools
// ============================================================

void register_builtin_tools(tool::ToolRegistry& registry) {
    registry.register_tool(std::make_shared<tool::FileReadTool>());
    registry.register_tool(std::make_shared<tool::FileWriteTool>());
    registry.register_tool(std::make_shared<tool::FileEditTool>());
    registry.register_tool(std::make_shared<tool::BashTool>());
    registry.register_tool(std::make_shared<tool::GlobTool>());
    registry.register_tool(std::make_shared<tool::GrepTool>());
    registry.register_tool(std::make_shared<tool::AskUserTool>());

    // Windows 平台额外注册 PowerShellTool（对齐 Claude Code 的条件注册策略）
    // BashTool（cmd.exe）和 PowerShellTool 并存，由模型根据任务特征自行选用
#ifdef _WIN32
    registry.register_tool(std::make_shared<tool::PowerShellTool>());
#endif
}

// ============================================================
// build_system_prompt
// ============================================================

namespace {

/// @brief 获取平台标识字符串（对齐 cc env.platform）
std::string get_platform_string() {
#ifdef _WIN32
    return "win32";
#elif defined(__APPLE__)
    return "darwin";
#else
    return "linux";
#endif
}

/// @brief 获取 OS 版本字符串（对齐 cc getUnameSR）
std::string get_os_version_string() {
#ifdef _WIN32
    // Windows: 用 RtlGetVersion 获取友好的版本名（避免 GetVersionEx 的 lie 模式）
    OSVERSIONINFOEXW osvi{};
    osvi.dwOSVersionInfoSize = sizeof(osvi);
    // RtlGetVersion 不受 manifest 影响，返回真实版本
    typedef LONG(WINAPI* RtlGetVersionPtr)(OSVERSIONINFOEXW*);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        auto pRtlGetVersion = reinterpret_cast<RtlGetVersionPtr>(
            GetProcAddress(ntdll, "RtlGetVersion"));
        if (pRtlGetVersion && pRtlGetVersion(&osvi) == 0) {
            // 粗略判定 Windows 版本名
            const char* edition = "Windows";
            if (osvi.dwMajorVersion == 10) {
                edition = osvi.dwBuildNumber >= 22000 ? "Windows 11" : "Windows 10";
            } else if (osvi.dwMajorVersion == 6 && osvi.dwMinorVersion == 3) {
                edition = "Windows 8.1";
            } else if (osvi.dwMajorVersion == 6 && osvi.dwMinorVersion == 2) {
                edition = "Windows 8";
            } else if (osvi.dwMajorVersion == 6 && osvi.dwMinorVersion == 1) {
                edition = "Windows 7";
            }
            return std::format("{} (build {})", edition, osvi.dwBuildNumber);
        }
    }
    return "Windows (unknown version)";
#else
    // POSIX: 用 uname -sr 等价信息
    struct utsname buf;
    if (uname(&buf) == 0) {
        return std::format("{} {}", buf.sysname, buf.release);
    }
    return "Unknown";
#endif
}

/// @brief 获取 shell 信息行（对齐 cc getShellInfoLine）
/// @details 通过 shell_detector 获取 BashTool 实际使用的 shell，
///          保证 env 段与 BashTool 的 prompt 完全一致
std::string get_shell_info_line() {
    const auto& sh = tool::shell_detect::detect();
    std::string bash_desc;
    if (sh.type == tool::shell_detect::ShellType::GitBash) {
        bash_desc = "Git Bash (" + sh.cmd + ")";
    } else if (sh.type == tool::shell_detect::ShellType::CmdExe) {
        bash_desc = "cmd.exe (degraded — Git Bash not found)";
    } else {
        // UnixSh
        bash_desc = sh.cmd;
    }

#ifdef _WIN32
    return bash_desc + " + PowerShell (powershell.exe)";
#else
    return bash_desc;
#endif
}

/// @brief 检测当前目录是否为 git 仓库
bool is_git_repo() {
    namespace fs = std::filesystem;
    fs::path cwd = fs::current_path();
    // 向上查找 .git 目录或文件
    for (fs::path p = cwd; !p.empty(); p = p.parent_path()) {
        if (fs::exists(p / ".git")) return true;
        if (p == p.parent_path()) break;
    }
    return false;
}

/// @brief 构建环境上下文段（对齐 cc computeEnvInfo 的 <env> 块）
std::string build_environment_context() {
    namespace fs = std::filesystem;
    std::string cwd = fs::current_path().string();
    std::string platform = get_platform_string();
    std::string os_version = get_os_version_string();
    std::string shell_line = get_shell_info_line();
    bool git_repo = is_git_repo();

    std::string env_block = std::format(
        "# Environment\n"
        "You are running on {}.\n"
        "- Platform: {}\n"
        "- Working directory: {}\n"
        "- Is directory a git repo: {}\n"
        "- Available shells: {}\n",
        os_version, platform, cwd,
        git_repo ? "Yes" : "No",
        shell_line);

    // Windows 平台补充 shell 选择指引
#ifdef _WIN32
    const auto& sh = tool::shell_detect::detect();
    if (sh.type == tool::shell_detect::ShellType::GitBash) {
        env_block +=
            "\n"
            "## Shell selection on Windows\n"
            "- **Bash tool** uses **Git Bash** — Unix commands (ls/grep/cat) are available.\n"
            "- **PowerShell tool** uses **powershell.exe** — for Windows-specific APIs "
            "(registry, WMI, .NET) and cmdlet pipelines.\n";
    } else {
        env_block +=
            "\n"
            "## Shell selection on Windows\n"
            "- **Bash tool** uses **cmd.exe** (degraded — Git Bash not found). "
            "Use Windows commands (dir, findstr, type, where). "
            "Unix commands like `ls`/`grep`/`cat` will FAIL.\n"
            "- **PowerShell tool** uses **powershell.exe** — supports aliases for "
            "ls/cat/cp/mv/rm. Prefer PowerShell for Unix-style commands on Windows.\n";
    }
#endif
    return env_block;
}

} // anonymous namespace

std::string build_system_prompt(const std::string& user_prompt,
                                const tool::ToolRegistry& registry) {
    std::string sys_prompt = user_prompt;

    // 注入环境上下文（<env> 段，对齐 Claude Code）
    sys_prompt += "\n\n";
    sys_prompt += build_environment_context();

    // 注入项目记忆（CLAUDE.md / AGENT.md，从 CWD 向上遍历）
    // 放在环境上下文之后、工具 prompt 之前，让项目约定优先级高于工具说明
    std::string project_memory = prompt::load_and_format_project_memory(std::filesystem::current_path());
    if (!project_memory.empty()) {
        sys_prompt += "\n\n";
        sys_prompt += project_memory;
    }

    // 拼接工具 prompt
    for (const auto& t : registry.get_all_tools()) {
        sys_prompt += "\n\n";
        sys_prompt += t->prompt();
    }

    // @file 引用说明
    sys_prompt +=
        "\n\n"
        "用户消息中可能出现 <file path=\"...\">...</file> 标签，这是用户通过 "
        "@path 语法引用的文件内容，已由前端读取并注入。对此类标签内的路径，"
        "禁止再次调用 Read 工具读取；直接基于标签内已有内容回答用户问题。";

    return sys_prompt;
}

} // namespace agent
