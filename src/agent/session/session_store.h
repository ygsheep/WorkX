/**
 * @file session_store.h
 * @brief JSONL 会话存储（每条消息实时追加）
 * @details 参考 cc 的 .jsonl 格式，每行一个 JSON 事件。
 *          写入时 open + append，读取时逐行解析。
 *
 *          事件类型：
 *          - session_start：会话元信息（cwd/model/gitBranch/createdAt）
 *          - user：用户消息
 *          - assistant：助手消息（含 reasoning_content / tool_uses）
 *          - tool：工具结果消息
 *          - session_end：会话结束标记
 *
 *          存储路径：~/.workx/projects/<编码路径>/<sessionId>.jsonl
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <vector>
#include <optional>
#include <fstream>
#include <filesystem>

#include <nlohmann/json.hpp>

#include "agent/api/chat_types.h"

namespace agent::session {

/// @brief 会话元信息（用于列表展示）
struct SessionMeta {
    std::string session_id;        ///< 会话 ID
    std::string file_path;         ///< JSONL 文件路径
    std::string created_at;        ///< 创建时间（ISO 8601）
    std::string cwd;               ///< 会话工作目录
    std::string model;             ///< 模型名
    std::string git_branch;        ///< git 分支
    std::filesystem::file_time_type last_modified;  ///< 最后修改时间（用于排序）
    int message_count = 0;         ///< 消息数（不含 session_start/end）
};

/// @brief JSONL 会话存储（每条消息实时追加）
/// @details 写入时 open + append（flush 保证崩溃不丢），读取时逐行解析。
class SessionStore {
public:
    /// @brief 构造
    /// @param file_path JSONL 文件路径
    /// @param session_id 会话 ID（写入 session_start/end 事件时使用）
    explicit SessionStore(std::string file_path, std::string session_id = "");

    ~SessionStore();

    /// @brief 打开文件（追加模式，不存在则创建）
    /// @return true=成功
    bool open();

    /// @brief 关闭文件
    void close();

    /// @brief 设置会话 ID（用于 session_start/end 事件）
    void set_session_id(std::string id) { m_session_id = std::move(id); }

    /// @brief 追加 session_start 事件
    bool append_session_start(const std::string& cwd,
                              const std::string& model,
                              const std::string& git_branch);

    /// @brief 追加 user 消息
    bool append_user_message(const std::string& uuid,
                             const std::string& parent_uuid,
                             const std::string& content,
                             const std::string& timestamp);

    /// @brief 追加 assistant 消息
    bool append_assistant_message(const std::string& uuid,
                                  const std::string& parent_uuid,
                                  const std::string& content,
                                  const std::string& reasoning_content,
                                  const std::vector<ToolUse>& tool_uses,
                                  const std::string& timestamp);

    /// @brief 追加 tool 消息
    bool append_tool_message(const std::string& uuid,
                             const std::string& parent_uuid,
                             const std::string& tool_call_id,
                             const std::string& tool_name,
                             const std::string& content,
                             bool is_error,
                             const std::string& timestamp);

    /// @brief 追加 session_end 事件
    bool append_session_end();

    // ============================================================
    // 静态工具方法
    // ============================================================

    /// @brief 读取 JSONL 文件所有事件
    static std::vector<nlohmann::json> read_all(const std::string& file_path);

    /// @brief 列出项目目录下的所有会话（按修改时间倒序）
    /// @param project_dir 项目目录路径
    /// @return 会话元信息列表
    static std::vector<SessionMeta> list_sessions(const std::string& project_dir);

    /// @brief 从 JSONL 文件加载消息历史（过滤掉 session_start/end 事件）
    /// @return ChatMessage 列表
    static std::vector<ChatMessage> load_messages(const std::string& file_path);

    /// @brief 从 JSONL 文件加载会话元信息
    static std::optional<SessionMeta> load_meta(const std::string& file_path);

private:
    std::string m_file_path;
    std::ofstream m_out;
    std::string m_session_id;

    bool append_line(const nlohmann::json& j);
};

/// @brief 获取项目会话目录路径
/// @param config_dir 配置根目录（如 ~/.workx）
/// @param cwd 项目工作目录
/// @return <config_dir>/projects/<编码路径>/
std::filesystem::path get_project_session_dir(const std::filesystem::path& config_dir,
                                              const std::string& cwd);

} // namespace agent::session
