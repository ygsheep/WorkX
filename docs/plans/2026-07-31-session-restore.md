# 项目会话恢复实现计划

> **For Codex:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 实现项目级会话持久化与恢复，参考 Claude Code 的 JSONL 事件流方案，在 `~/.workx/projects/<编码路径>/<sessionId>.jsonl` 存储会话，启动时询问用户是否恢复上次会话。

**Architecture:** 三层架构：(1) 底层 UUID + 路径编码工具；(2) 中层 JSONL SessionStore 实时追加写入与加载；(3) 上层 ChatSession 集成 + TUI 恢复询问流程。配置目录从 `AppData/Roaming/workx` 迁移到 `~/.workx`。

**Tech Stack:** C++20、std::filesystem、nlohmann::json、Catch2、ftxui（TUI 弹窗）

---

## 设计决策（已与用户确认）

| 决策点 | 选择 |
|--------|------|
| 旧配置迁移 | 不迁移，新目录从零开始 |
| 恢复策略 | 启动时询问用户（TUI 弹窗） |
| 写入时机 | 每条消息实时追加到 JSONL |
| 路径编码 | 全替换为 `-`（对齐 cc，如 `D--develop-Workspace-workx`） |
| 配置目录 | Windows: `%USERPROFILE%\.workx`；POSIX: `$HOME/.workx` |

---

## 存储结构

```
~/.workx/
├── config.json                          # 全局配置
├── logs/                                # 日志
│   └── workx_YYYYMMDD_HHMMSS.log
└── projects/                            # 项目级会话
    └── D--develop-Workspace-workx/      # 项目路径编码（分隔符→'-'）
        ├── 76e1b10d-dcf4-4a08-ac3f-3d1290dddbef.jsonl
        └── a3f2c890-1234-5678-9abc-def012345678.jsonl
```

## JSONL 事件格式

每行一个 JSON 事件，`type` 字段区分类型：

```jsonl
{"type":"session_start","sessionId":"...","cwd":"...","createdAt":"2026-07-31T...","model":"...","gitBranch":"..."}
{"type":"user","uuid":"...","parentUuid":"...","timestamp":"...","content":"..."}
{"type":"assistant","uuid":"...","parentUuid":"...","timestamp":"...","content":"...","reasoningContent":"...","toolUses":[...]}
{"type":"tool","uuid":"...","parentUuid":"...","timestamp":"...","toolCallId":"...","toolName":"...","content":"...","isError":false}
{"type":"session_end","sessionId":"...","endedAt":"..."}
```

---

## Task 1: 修改配置目录到 ~/.workx

**Files:**
- Modify: `src/app/config/app_config.cpp:218-245`

**Step 1: 修改 get_config_dir()**

将 Windows 优先级从 `APPDATA/workx` 改为 `USERPROFILE/.workx`：

```cpp
static std::filesystem::path get_config_dir() {
    // 1. 显式环境变量优先
    if (const char* env = std::getenv("WORKX_CONFIG_DIR")) {
        if (env[0] != '\0') return std::filesystem::path(env);
    }
#ifdef _WIN32
    // Windows: %USERPROFILE%\.workx（对齐 cc 的 ~/.claude 风格）
    if (const char* env = std::getenv("USERPROFILE")) {
        if (env[0] != '\0') return std::filesystem::path(env) / ".workx";
    }
    // 回退：APPDATA（某些受限环境 USERPROFILE 可能缺失）
    if (const char* env = std::getenv("APPDATA")) {
        if (env[0] != '\0') return std::filesystem::path(env) / "workx";
    }
#else
    // POSIX: $HOME/.workx
    if (const char* env = std::getenv("HOME")) {
        if (env[0] != '\0') return std::filesystem::path(env) / ".workx";
    }
    if (const char* env = std::getenv("XDG_CONFIG_HOME")) {
        if (env[0] != '\0') return std::filesystem::path(env) / "workx";
    }
#endif
    return std::filesystem::current_path() / ".workx";
}
```

**Step 2: 验证**

运行程序，确认日志出现在 `~/.workx/logs/`，config.json 读取自 `~/.workx/config.json`。

**Step 3: Commit**

```bash
git add src/app/config/app_config.cpp
git commit -m "feat(config): 迁移配置目录到 ~/.workx

Windows: %USERPROFILE%\.workx（原 %APPDATA%\workx）
POSIX: $HOME/.workx（原 $XDG_CONFIG_HOME/workx）

对齐 cc 的 ~/.claude 风格，为项目级会话存储做准备。"
```

---

## Task 2: 新增 UUID 生成工具

**Files:**
- Create: `src/core/util/uuid.h`
- Create: `src/core/util/uuid.cpp`
- Modify: `src/core/CMakeLists.txt`（添加 util/uuid.cpp）
- Create: `tests/unit/core/util/test_uuid.cpp`
- Modify: `tests/unit/core/CMakeLists.txt`

**Step 1: 写失败测试**

```cpp
// tests/unit/core/util/test_uuid.cpp
#include <catch2/catch_test_macros.hpp>
#include <regex>
#include "core/util/uuid.h"

TEST_CASE("uuid: generates valid UUIDv4 format", "[core][uuid]") {
    std::string id = core::util::generate_uuid();
    // UUIDv4 格式：xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx（y 为 8/9/a/b）
    std::regex re("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$");
    REQUIRE(std::regex_match(id, re));
}

TEST_CASE("uuid: generates unique values", "[core][uuid]") {
    std::string a = core::util::generate_uuid();
    std::string b = core::util::generate_uuid();
    REQUIRE(a != b);
}

TEST_CASE("uuid: empty string never returned", "[core][uuid]") {
    for (int i = 0; i < 100; ++i) {
        REQUIRE_FALSE(core::util::generate_uuid().empty());
    }
}
```

**Step 2: 运行测试确认失败**

Run: `cmake --build build --config Release --target workx_unit_tests && .\build\bin\Release\workx_unit_tests.exe "[uuid]"`

**Step 3: 实现**

```cpp
// src/core/util/uuid.h
#pragma once
#include <string>

namespace core::util {

/// @brief 生成 UUIDv4 字符串（小写，带连字符）
/// @return 形如 "550e8400-e29b-41d4-a716-446655440000"
/// @details 使用 std::random_device + mt19937 生成随机数，
///          符合 RFC 4122 第 4.4 节（random UUID）。
std::string generate_uuid();

} // namespace core::util
```

```cpp
// src/core/util/uuid.cpp
#include "core/util/uuid.h"
#include <random>
#include <format>

namespace core::util {

std::string generate_uuid() {
    // 使用 random_device 播种 mt19937（每次调用独立，避免全局状态）
    std::random_device rd;
    std::mt19937_64 gen(rd());
    
    // 生成 128 位随机数（两个 64 位）
    uint64_t hi = gen();
    uint64_t lo = gen();
    
    // 按 RFC 4122 §4.4 设置版本与变体位
    // 版本：第 7 字节高 4 位 = 0100 (4)
    uint32_t time_hi = static_cast<uint32_t>((hi >> 48) & 0xFFFF);
    time_hi = (time_hi & 0x0FFF) | 0x4000;  // 版本 4
    // 变体：第 9 字节高 2 位 = 10
    uint16_t clock_seq = static_cast<uint16_t>((hi >> 32) & 0xFFFF);
    clock_seq = (clock_seq & 0x3FFF) | 0x8000;  // 变体 10xx
    
    uint32_t time_low = static_cast<uint32_t>(hi & 0xFFFFFFFF);
    uint16_t time_mid = static_cast<uint16_t>((hi >> 16) & 0xFFFF);
    uint64_t node = lo;
    
    return std::format("{:08x}-{:04x}-{:04x}-{:04x}-{:012x}",
                       time_low, time_mid, time_hi, clock_seq, node);
}

} // namespace core::util
```

**Step 4: 运行测试确认通过**

Run: `cmake --build build --config Release --target workx_unit_tests && .\build\bin\Release\workx_unit_tests.exe "[uuid]"`

**Step 5: Commit**

```bash
git add src/core/util/uuid.h src/core/util/uuid.cpp src/core/CMakeLists.txt tests/unit/core/util/test_uuid.cpp tests/unit/core/CMakeLists.txt
git commit -m "feat(util): 新增 UUIDv4 生成工具

为会话 ID 生成提供基础。使用 std::random_device + mt19937_64，
符合 RFC 4122 §4.4（random UUID）。"
```

---

## Task 3: 新增项目路径编码工具

**Files:**
- Create: `src/core/util/path_encoder.h`
- Create: `src/core/util/path_encoder.cpp`
- Modify: `src/core/CMakeLists.txt`
- Create: `tests/unit/core/util/test_path_encoder.cpp`

**Step 1: 写失败测试**

```cpp
// tests/unit/core/util/test_path_encoder.cpp
#include <catch2/catch_test_macros.hpp>
#include "core/util/path_encoder.h"

TEST_CASE("path_encoder: encodes Windows path", "[core][path_encoder]") {
    // 路径分隔符 \ 和 : 全替换为 -
    REQUIRE(core::util::encode_project_path(R"(D:\develop\Workspace\workx)") 
            == "D--develop-Workspace-workx");
}

TEST_CASE("path_encoder: encodes POSIX path", "[core][path_encoder]") {
    REQUIRE(core::util::encode_project_path("/home/user/workx") 
            == "-home-user-workx");
}

TEST_CASE("path_encoder: handles trailing separator", "[core][path_encoder]") {
    REQUIRE(core::util::encode_project_path(R"(D:\develop\workx\)") 
            == "D--develop-workx-");
}

TEST_CASE("path_encoder: empty path returns empty", "[core][path_encoder]") {
    REQUIRE(core::util::encode_project_path("").empty());
}
```

**Step 2: 运行测试确认失败**

**Step 3: 实现**

```cpp
// src/core/util/path_encoder.h
#pragma once
#include <string>
#include <filesystem>

namespace core::util {

/// @brief 将项目路径编码为目录名（对齐 cc 的 projects 目录命名）
/// @param path 项目绝对路径
/// @return 编码后的字符串（路径分隔符 \ / 和盘符冒号 : 全替换为 -）
/// @details 例：D:\develop\workx → D--develop-workx
///          /home/user/workx → -home-user-workx
std::string encode_project_path(const std::filesystem::path& path);

} // namespace core::util
```

```cpp
// src/core/util/path_encoder.cpp
#include "core/util/path_encoder.h"
#include <algorithm>

namespace core::util {

std::string encode_project_path(const std::filesystem::path& path) {
    std::string s = path.string();
    if (s.empty()) return s;
    std::replace(s.begin(), s.end(), '\\', '-');
    std::replace(s.begin(), s.end(), '/', '-');
    std::replace(s.begin(), s.end(), ':', '-');
    return s;
}

} // namespace core::util
```

**Step 4: 运行测试确认通过**

**Step 5: Commit**

```bash
git add src/core/util/path_encoder.h src/core/util/path_encoder.cpp tests/unit/core/util/test_path_encoder.cpp
git commit -m "feat(util): 新增项目路径编码工具

路径分隔符(\/)和盘符冒号(:)全替换为'-'，
对齐 cc 的 projects 目录命名方案。"
```

---

## Task 4: 新增 JSONL SessionStore

**Files:**
- Create: `src/agent/session/session_store.h`
- Create: `src/agent/session/session_store.cpp`
- Modify: `src/agent/CMakeLists.txt`
- Create: `tests/unit/agent/session/test_session_store.cpp`

**Step 1: 写失败测试**

```cpp
// tests/unit/agent/session/test_session_store.cpp
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include "agent/session/session_store.h"

using namespace agent::session;

TEST_CASE("session_store: append and read back", "[session][store]") {
    auto tmp = std::filesystem::temp_directory_path() / "workx_test_session.jsonl";
    std::filesystem::remove(tmp);
    
    SessionStore store(tmp.string());
    REQUIRE(store.open());
    
    // 写入 session_start
    REQUIRE(store.append_session_start("cwd", "model", "branch"));
    // 写入 user 消息
    REQUIRE(store.append_user_message("u1", "", "hello", "2026-07-31T10:00:00Z"));
    // 写入 assistant 消息
    REQUIRE(store.append_assistant_message("a1", "u1", "hi there", "", {}, "2026-07-31T10:00:01Z"));
    // 写入 session_end
    REQUIRE(store.append_session_end());
    
    store.close();
    
    // 读取回来
    auto events = SessionStore::read_all(tmp.string());
    REQUIRE(events.size() == 4);
    REQUIRE(events[0]["type"] == "session_start");
    REQUIRE(events[0]["cwd"] == "cwd");
    REQUIRE(events[1]["type"] == "user");
    REQUIRE(events[1]["content"] == "hello");
    REQUIRE(events[2]["type"] == "assistant");
    REQUIRE(events[2]["content"] == "hi there");
    REQUIRE(events[2]["parentUuid"] == "u1");
    REQUIRE(events[3]["type"] == "session_end");
    
    std::filesystem::remove(tmp);
}

TEST_CASE("session_store: list sessions in project dir", "[session][store]") {
    auto tmp_dir = std::filesystem::temp_directory_path() / "workx_test_project";
    std::filesystem::remove_all(tmp_dir);
    std::filesystem::create_directories(tmp_dir);
    
    // 创建两个会话文件
    auto f1 = tmp_dir / "aaa-111.jsonl";
    auto f2 = tmp_dir / "bbb-222.jsonl";
    { std::ofstream(f1.string()) << R"({"type":"session_start","sessionId":"aaa-111","createdAt":"2026-07-30T10:00:00Z"})" << "\n"; }
    { std::ofstream(f2.string()) << R"({"type":"session_start","sessionId":"bbb-222","createdAt":"2026-07-31T10:00:00Z"})" << "\n"; }
    
    auto sessions = SessionStore::list_sessions(tmp_dir.string());
    REQUIRE(sessions.size() == 2);
    // 按修改时间倒序，bbb-222 应在前
    REQUIRE(sessions[0].session_id == "bbb-222");
    REQUIRE(sessions[1].session_id == "aaa-111");
    
    std::filesystem::remove_all(tmp_dir);
}

TEST_CASE("session_store: load messages from jsonl", "[session][store]") {
    auto tmp = std::filesystem::temp_directory_path() / "workx_test_load.jsonl";
    std::filesystem::remove(tmp);
    
    {
        std::ofstream f(tmp.string());
        f << R"({"type":"session_start","sessionId":"s1","cwd":"/tmp","model":"m","gitBranch":"main"})" << "\n";
        f << R"({"type":"user","uuid":"u1","parentUuid":"","timestamp":"t1","content":"hello"})" << "\n";
        f << R"({"type":"assistant","uuid":"a1","parentUuid":"u1","timestamp":"t2","content":"hi","reasoningContent":"","toolUses":[]})" << "\n";
        f << R"({"type":"tool","uuid":"t1","parentUuid":"a1","timestamp":"t3","toolCallId":"tc1","toolName":"Read","content":"file content","isError":false})" << "\n";
        f << R"({"type":"session_end","sessionId":"s1"})" << "\n";
    }
    
    auto messages = SessionStore::load_messages(tmp.string());
    REQUIRE(messages.size() == 3);  // user + assistant + tool（不含 session_start/end）
    REQUIRE(messages[0].role == ChatMessage::Role::User);
    REQUIRE(messages[0].content == "hello");
    REQUIRE(messages[1].role == ChatMessage::Role::Assistant);
    REQUIRE(messages[1].content == "hi");
    REQUIRE(messages[2].role == ChatMessage::Role::Tool);
    REQUIRE(messages[2].tool_call_id == "tc1");
    REQUIRE(messages[2].tool_name == "Read");
    REQUIRE(messages[2].content == "file content");
    
    std::filesystem::remove(tmp);
}

TEST_CASE("session_store: already_elided message is idempotent", "[session][store]") {
    // 验证重复 open 不清空文件
    auto tmp = std::filesystem::temp_directory_path() / "workx_test_append.jsonl";
    std::filesystem::remove(tmp);
    
    SessionStore store1(tmp.string());
    REQUIRE(store1.open());
    REQUIRE(store1.append_user_message("u1", "", "first", "t1"));
    store1.close();
    
    SessionStore store2(tmp.string());
    REQUIRE(store2.open());
    REQUIRE(store2.append_user_message("u2", "", "second", "t2"));
    store2.close();
    
    auto events = SessionStore::read_all(tmp.string());
    REQUIRE(events.size() == 2);
    REQUIRE(events[0]["content"] == "first");
    REQUIRE(events[1]["content"] == "second");
    
    std::filesystem::remove(tmp);
}
```

**Step 2: 运行测试确认失败**

**Step 3: 实现**

```cpp
// src/agent/session/session_store.h
#pragma once

#include <string>
#include <vector>
#include <optional>
#include <fstream>
#include <filesystem>
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
/// @details 参考 cc 的 .jsonl 格式，每行一个 JSON 事件。
///          写入时 open + append，读取时逐行解析。
class SessionStore {
public:
    /// @brief 构造
    /// @param file_path JSONL 文件路径
    explicit SessionStore(std::string file_path);
    
    ~SessionStore();
    
    /// @brief 打开文件（追加模式，不存在则创建）
    /// @return true=成功
    bool open();
    
    /// @brief 关闭文件
    void close();
    
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
```

```cpp
// src/agent/session/session_store.cpp
#include "agent/session/session_store.h"
#include "core/util/path_encoder.h"
#include <nlohmann/json.hpp>
#include <iostream>
#include <chrono>
#include <format>

namespace agent::session {

namespace {

/// @brief 获取当前 ISO 8601 时间戳
std::string now_iso() {
    auto now = std::chrono::system_clock::now();
    auto t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

} // anonymous namespace

SessionStore::SessionStore(std::string file_path)
    : m_file_path(std::move(file_path)) {}

SessionStore::~SessionStore() {
    close();
}

bool SessionStore::open() {
    // 确保父目录存在
    auto parent = std::filesystem::path(m_file_path).parent_path();
    if (!parent.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(parent, ec);
    }
    // 追加模式打开（不截断已有内容）
    m_out.open(m_file_path, std::ios::app);
    return m_out.is_open();
}

void SessionStore::close() {
    if (m_out.is_open()) {
        m_out.flush();
        m_out.close();
    }
}

bool SessionStore::append_line(const nlohmann::json& j) {
    if (!m_out.is_open()) return false;
    m_out << j.dump() << "\n";
    m_out.flush();  // 实时刷盘，崩溃不丢消息
    return m_out.good();
}

bool SessionStore::append_session_start(const std::string& cwd,
                                        const std::string& model,
                                        const std::string& git_branch) {
    nlohmann::json j;
    j["type"] = "session_start";
    j["sessionId"] = m_session_id;
    j["cwd"] = cwd;
    j["model"] = model;
    j["gitBranch"] = git_branch;
    j["createdAt"] = now_iso();
    return append_line(j);
}

bool SessionStore::append_user_message(const std::string& uuid,
                                       const std::string& parent_uuid,
                                       const std::string& content,
                                       const std::string& timestamp) {
    nlohmann::json j;
    j["type"] = "user";
    j["uuid"] = uuid;
    j["parentUuid"] = parent_uuid;
    j["timestamp"] = timestamp;
    j["content"] = content;
    return append_line(j);
}

bool SessionStore::append_assistant_message(const std::string& uuid,
                                            const std::string& parent_uuid,
                                            const std::string& content,
                                            const std::string& reasoning_content,
                                            const std::vector<ToolUse>& tool_uses,
                                            const std::string& timestamp) {
    nlohmann::json j;
    j["type"] = "assistant";
    j["uuid"] = uuid;
    j["parentUuid"] = parent_uuid;
    j["timestamp"] = timestamp;
    j["content"] = content;
    j["reasoningContent"] = reasoning_content;
    nlohmann::json uses = nlohmann::json::array();
    for (const auto& u : tool_uses) {
        uses.push_back({{"id", u.id}, {"name", u.name}, {"input", u.input}});
    }
    j["toolUses"] = uses;
    return append_line(j);
}

bool SessionStore::append_tool_message(const std::string& uuid,
                                       const std::string& parent_uuid,
                                       const std::string& tool_call_id,
                                       const std::string& tool_name,
                                       const std::string& content,
                                       bool is_error,
                                       const std::string& timestamp) {
    nlohmann::json j;
    j["type"] = "tool";
    j["uuid"] = uuid;
    j["parentUuid"] = parent_uuid;
    j["timestamp"] = timestamp;
    j["toolCallId"] = tool_call_id;
    j["toolName"] = tool_name;
    j["content"] = content;
    j["isError"] = is_error;
    return append_line(j);
}

bool SessionStore::append_session_end() {
    nlohmann::json j;
    j["type"] = "session_end";
    j["sessionId"] = m_session_id;
    j["endedAt"] = now_iso();
    return append_line(j);
}

// ============================================================
// 静态方法
// ============================================================

std::vector<nlohmann::json> SessionStore::read_all(const std::string& file_path) {
    std::vector<nlohmann::json> events;
    std::ifstream in(file_path);
    if (!in.is_open()) return events;
    
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        try {
            events.push_back(nlohmann::json::parse(line));
        } catch (const std::exception&) {
            // 跳过损坏的行（可能是崩溃时写了一半）
            continue;
        }
    }
    return events;
}

std::vector<SessionMeta> SessionStore::list_sessions(const std::string& project_dir) {
    std::vector<SessionMeta> sessions;
    std::error_code ec;
    if (!std::filesystem::exists(project_dir, ec)) return sessions;
    
    for (const auto& entry : std::filesystem::directory_iterator(project_dir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".jsonl") continue;
        
        auto meta = load_meta(entry.path().string());
        if (meta) {
            meta->file_path = entry.path().string();
            meta->last_modified = entry.last_write_time(ec);
            sessions.push_back(*meta);
        }
    }
    
    // 按修改时间倒序（最新在前）
    std::sort(sessions.begin(), sessions.end(),
              [](const SessionMeta& a, const SessionMeta& b) {
                  return a.last_modified > b.last_modified;
              });
    return sessions;
}

std::vector<ChatMessage> SessionStore::load_messages(const std::string& file_path) {
    std::vector<ChatMessage> messages;
    auto events = read_all(file_path);
    
    for (const auto& j : events) {
        std::string type = j.value("type", "");
        if (type == "user") {
            ChatMessage msg = ChatMessage::user(j.value("content", ""));
            messages.push_back(msg);
        } else if (type == "assistant") {
            ChatMessage msg = ChatMessage::assistant(j.value("content", ""));
            msg.reasoning_content = j.value("reasoningContent", "");
            if (j.contains("toolUses") && j["toolUses"].is_array()) {
                for (const auto& u : j["toolUses"]) {
                    ToolUse tu;
                    tu.id = u.value("id", "");
                    tu.name = u.value("name", "");
                    tu.input = u.value("input", nlohmann::json::object());
                    msg.tool_uses.push_back(tu);
                }
            }
            messages.push_back(msg);
        } else if (type == "tool") {
            ChatMessage msg = ChatMessage::tool_result(
                j.value("toolCallId", ""),
                j.value("toolName", ""),
                j.value("content", ""),
                j.value("isError", false)
            );
            messages.push_back(msg);
        }
        // session_start / session_end 不转为消息
    }
    return messages;
}

std::optional<SessionMeta> SessionStore::load_meta(const std::string& file_path) {
    auto events = read_all(file_path);
    if (events.empty()) return std::nullopt;
    
    SessionMeta meta;
    int msg_count = 0;
    
    for (const auto& j : events) {
        std::string type = j.value("type", "");
        if (type == "session_start") {
            meta.session_id = j.value("sessionId", "");
            meta.cwd = j.value("cwd", "");
            meta.model = j.value("model", "");
            meta.git_branch = j.value("gitBranch", "");
            meta.created_at = j.value("createdAt", "");
        } else if (type == "user" || type == "assistant" || type == "tool") {
            ++msg_count;
        }
    }
    meta.message_count = msg_count;
    
    if (meta.session_id.empty()) {
        // 没有 session_start 事件，用文件名作为 session_id
        meta.session_id = std::filesystem::path(file_path).stem().string();
    }
    
    return meta;
}

std::filesystem::path get_project_session_dir(const std::filesystem::path& config_dir,
                                              const std::string& cwd) {
    return config_dir / "projects" / core::util::encode_project_path(cwd);
}

} // namespace agent::session
```

**Step 4: 运行测试确认通过**

**Step 5: Commit**

```bash
git add src/agent/session/ tests/unit/agent/session/ src/agent/CMakeLists.txt tests/unit/agent/CMakeLists.txt
git commit -m "feat(session): 新增 JSONL SessionStore

- 每条消息实时 append 到 JSONL（flush 保证崩溃不丢）
- 支持 session_start/user/assistant/tool/session_end 事件
- list_sessions 按修改时间倒序列出项目下所有会话
- load_messages 从 JSONL 恢复 ChatMessage 列表"
```

---

## Task 5: ChatSession 集成 SessionStore

**Files:**
- Modify: `src/agent/core/chat_session.h`
- Modify: `src/agent/core/chat_session.cpp`

**Step 1: 修改 ChatSession**

在 ChatSession 中添加 SessionStore 成员和写入钩子：

```cpp
// chat_session.h 新增
#include "agent/session/session_store.h"

class ChatSession {
public:
    /// @brief 设置 SessionStore（可选，设置后每条消息实时持久化）
    void set_session_store(std::shared_ptr<agent::session::SessionStore> store);
    
    /// @brief 从 JSONL 文件加载历史会话
    /// @param file_path JSONL 文件路径
    /// @return true=加载成功
    bool restore_from_file(const std::string& file_path);
    
private:
    std::shared_ptr<agent::session::SessionStore> m_session_store;
    
    /// @brief 持久化单条消息到 SessionStore（如果已设置）
    void persist_message(const ChatMessage& msg);
};
```

```cpp
// chat_session.cpp 实现
void ChatSession::set_session_store(std::shared_ptr<agent::session::SessionStore> store) {
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_session_store = std::move(store);
}

void ChatSession::persist_message(const ChatMessage& msg) {
    if (!m_session_store) return;
    
    std::string uuid = core::util::generate_uuid();
    std::string timestamp = /* ISO 8601 now */;
    
    switch (msg.role) {
        case ChatMessage::Role::User:
            m_session_store->append_user_message(uuid, "", msg.content, timestamp);
            break;
        case ChatMessage::Role::Assistant:
            m_session_store->append_assistant_message(uuid, "", msg.content,
                                                       msg.reasoning_content,
                                                       msg.tool_uses, timestamp);
            break;
        case ChatMessage::Role::Tool:
            m_session_store->append_tool_message(uuid, "", msg.tool_call_id,
                                                  msg.tool_name, msg.content,
                                                  msg.is_error, timestamp);
            break;
        default:
            break;
    }
}

bool ChatSession::restore_from_file(const std::string& file_path) {
    auto messages = agent::session::SessionStore::load_messages(file_path);
    if (messages.empty()) return false;
    
    std::lock_guard<std::mutex> lock(m_state_mutex);
    m_messages = std::move(messages);
    return true;
}
```

**Step 2: 在消息追加点调用 persist_message**

找到所有 `m_messages.push_back(...)` 的位置，在 push_back 后调用 `persist_message(msg)`。

**Step 3: Commit**

```bash
git commit -m "feat(session): ChatSession 集成 SessionStore 实时持久化

- set_session_store 注入后，每条消息 push_back 同时 append 到 JSONL
- restore_from_file 从 JSONL 恢复历史消息到 m_messages"
```

---

## Task 6: factory.cpp 注入 SessionStore

**Files:**
- Modify: `src/app/factory.cpp`

**Step 1: 修改 create_session**

```cpp
// factory.cpp 在 ChatSession 构造后注入 SessionStore
#include "agent/session/session_store.h"
#include "core/util/uuid.h"
#include "core/util/path_encoder.h"

// 生成 session_id（UUID）
std::string session_id = core::util::generate_uuid();

// 修改 ChatSession 构造，传入真实 session_id
result.session = std::make_unique<ChatSession>(
    std::move(backend), task_manager, event_bus, cfg,
    default_retry_delay, session_id);

// 创建 SessionStore
auto config_dir = agent::default_config_path().parent_path();
auto project_dir = agent::session::get_project_session_dir(
    config_dir, std::filesystem::current_path().string());
auto session_file = project_dir / (session_id + ".jsonl");

auto store = std::make_shared<agent::session::SessionStore>(session_file.string());
store->open();
// 注意：需要让 SessionStore 知道 session_id（通过 setter 或构造参数）
store->append_session_start(/* cwd */, /* model */, /* git_branch */);

result.session->set_session_store(store);
```

**Step 2: Commit**

---

## Task 7: 启动时询问恢复流程

**Files:**
- Modify: `src/app/main.cpp`
- Create: `src/app/session_restore.h`
- Create: `src/app/session_restore.cpp`

**Step 1: 实现 session_restore**

```cpp
// session_restore.h
#pragma once
#include <optional>
#include <string>
#include "agent/session/session_store.h"

namespace agent {

/// @brief 启动时检查并询问用户是否恢复上次会话
/// @param project_dir 项目会话目录
/// @return 选中的会话文件路径（用户选择开新会话则返回空）
std::optional<std::string> prompt_restore_session(
    const std::string& project_dir);

} // namespace agent
```

```cpp
// session_restore.cpp
#include "session_restore.h"
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>

namespace agent {

std::optional<std::string> prompt_restore_session(const std::string& project_dir) {
    auto sessions = session::SessionStore::list_sessions(project_dir);
    
    if (sessions.empty()) {
        return std::nullopt;  // 无历史会话，直接开新会话
    }
    
    // 最多展示最近 5 条
    int show_count = std::min<int>(5, sessions.size());
    
    using namespace ftxui;
    auto screen = ScreenInteractive::Fullscreen();
    
    int selected = -1;
    
    Components entries;
    for (int i = 0; i < show_count; ++i) {
        const auto& s = sessions[i];
        std::string label = std::format("{} | {} | {} 条消息 | {}",
            s.created_at, s.git_branch, s.message_count, s.model);
        entries.push_back(Button(label, [&, i] { selected = i; screen.Exit(); }));
    }
    
    auto new_session_btn = Button("开始新会话", [&] { selected = -1; screen.Exit(); });
    
    auto layout = VStack({
        text("发现历史会话，是否恢复？") | bold | center,
        separator(),
        VBox(entries),
        separator(),
        new_session_btn,
    }) | border | center;
    
    screen.Loop(Component::from(layout));
    
    if (selected >= 0 && selected < show_count) {
        return sessions[selected].file_path;
    }
    return std::nullopt;
}

} // namespace agent
```

**Step 2: main.cpp 集成**

在 `create_session` 后、主循环前插入恢复逻辑：

```cpp
// main.cpp
auto project_dir = session::get_project_session_dir(
    config_dir, cwd).string();

// 询问用户是否恢复
auto restore_file = prompt_restore_session(project_dir);

if (restore_file) {
    // 恢复历史消息
    session->restore_from_file(*restore_file);
    // 复用旧 session_id（从文件名提取）
    // ...
} else {
    // 开新会话（factory 已创建新 SessionStore）
}
```

**Step 3: Commit**

---

## Task 8: 退出时写入 session_end

**Files:**
- Modify: `src/app/main.cpp`

**Step 1: 在程序退出前调用 append_session_end**

注册 atexit 或在 main 返回前调用：

```cpp
// main.cpp 退出前
if (session_store) {
    session_store->append_session_end();
    session_store->close();
}
```

**Step 2: Commit**

---

## Task 9: 更新序列化完整性（tool_uses）

**Files:**
- Modify: `src/agent/core/chat_session.cpp` 的 `serialize_state()` / `deserialize_state()`

**Step 1: 补齐 tool_uses 序列化**

现有 `serialize_state()` 未保存 `tool_uses`，导致 `/save` `/load` 后丢失。在 JSON 中增加 `tool_uses` 字段。

**Step 2: Commit**

---

## Task 10: 端到端测试与文档

**Files:**
- Create: `tests/unit/agent/session/test_e2e_restore.cpp`
- Update: `README.md`（如有相关章节）

**Step 1: 写端到端测试**

- 创建会话 → 写入若干消息 → 关闭
- 重新打开 → 验证消息完整恢复

**Step 2: 更新文档**

**Step 3: Final Commit**

---

## 验收标准

- [ ] 配置目录迁移到 `~/.workx`
- [ ] UUIDv4 生成工具通过测试
- [ ] 项目路径编码正确（`D:\develop\workx` → `D--develop-workx`）
- [ ] SessionStore 实时追加 + 读取通过测试
- [ ] ChatSession 每条消息持久化到 JSONL
- [ ] 启动时 TUI 弹窗列出历史会话，用户可选择恢复或开新会话
- [ ] 恢复后消息历史完整（含 tool_uses）
- [ ] 退出时写入 session_end
- [ ] 全量测试通过
