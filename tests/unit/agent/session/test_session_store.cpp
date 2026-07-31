/**
 * @file test_session_store.cpp
 * @brief JSONL SessionStore 单元测试
 */

#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>

#include "agent/session/session_store.h"

using namespace agent::session;

namespace {

/// @brief 临时文件 RAII 清理
class TempFile {
public:
    explicit TempFile(const std::string& name)
        : path_(std::filesystem::temp_directory_path() / name) {
        std::filesystem::remove(path_);
    }
    ~TempFile() { std::filesystem::remove(path_); }
    const std::filesystem::path& path() const { return path_; }
    std::string string() const { return path_.string(); }
private:
    std::filesystem::path path_;
};

class TempDir {
public:
    explicit TempDir(const std::string& name)
        : path_(std::filesystem::temp_directory_path() / name) {
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~TempDir() { std::filesystem::remove_all(path_); }
    const std::filesystem::path& path() const { return path_; }
    std::string string() const { return path_.string(); }
private:
    std::filesystem::path path_;
};

} // anonymous namespace

// ============================================================
// 追加与读取
// ============================================================

TEST_CASE("session_store: append and read back", "[session][store]") {
    TempFile tmp("workx_test_session.jsonl");

    SessionStore store(tmp.string(), "test-session-id");
    REQUIRE(store.open());

    REQUIRE(store.append_session_start("/cwd", "test-model", "main"));
    REQUIRE(store.append_user_message("u1", "", "hello", "2026-07-31T10:00:00Z"));
    REQUIRE(store.append_assistant_message("a1", "u1", "hi there", "", {}, "2026-07-31T10:00:01Z"));
    REQUIRE(store.append_session_end());

    store.close();

    auto events = SessionStore::read_all(tmp.string());
    REQUIRE(events.size() == 4);
    REQUIRE(events[0]["type"] == "session_start");
    REQUIRE(events[0]["cwd"] == "/cwd");
    REQUIRE(events[0]["sessionId"] == "test-session-id");
    REQUIRE(events[1]["type"] == "user");
    REQUIRE(events[1]["content"] == "hello");
    REQUIRE(events[2]["type"] == "assistant");
    REQUIRE(events[2]["content"] == "hi there");
    REQUIRE(events[2]["parentUuid"] == "u1");
    REQUIRE(events[3]["type"] == "session_end");
    REQUIRE(events[3]["sessionId"] == "test-session-id");
}

TEST_CASE("session_store: append is idempotent (no truncate)", "[session][store]") {
    // 验证重复 open 不清空文件
    TempFile tmp("workx_test_append.jsonl");

    {
        SessionStore store(tmp.string());
        REQUIRE(store.open());
        REQUIRE(store.append_user_message("u1", "", "first", "t1"));
        store.close();
    }

    {
        SessionStore store(tmp.string());
        REQUIRE(store.open());
        REQUIRE(store.append_user_message("u2", "", "second", "t2"));
        store.close();
    }

    auto events = SessionStore::read_all(tmp.string());
    REQUIRE(events.size() == 2);
    REQUIRE(events[0]["content"] == "first");
    REQUIRE(events[1]["content"] == "second");
}

// ============================================================
// assistant 消息含 tool_uses
// ============================================================

TEST_CASE("session_store: assistant with tool_uses round-trip", "[session][store]") {
    TempFile tmp("workx_test_tooluses.jsonl");

    std::vector<agent::ToolUse> uses;
    uses.push_back({"toolu_001", "Read", nlohmann::json({{"path", "/tmp/test"}})});
    uses.push_back({"toolu_002", "Write", nlohmann::json({{"path", "/tmp/out"}, {"content", "hi"}})});

    {
        SessionStore store(tmp.string());
        REQUIRE(store.open());
        REQUIRE(store.append_assistant_message("a1", "u1", "calling tools", "thinking...", uses, "t1"));
        store.close();
    }

    auto events = SessionStore::read_all(tmp.string());
    REQUIRE(events.size() == 1);
    REQUIRE(events[0]["toolUses"].size() == 2);
    REQUIRE(events[0]["toolUses"][0]["id"] == "toolu_001");
    REQUIRE(events[0]["toolUses"][0]["name"] == "Read");
    REQUIRE(events[0]["toolUses"][0]["input"]["path"] == "/tmp/test");
    REQUIRE(events[0]["toolUses"][1]["name"] == "Write");
}

// ============================================================
// list_sessions
// ============================================================

TEST_CASE("session_store: list sessions in project dir", "[session][store]") {
    TempDir tmp("workx_test_project");

    auto f1 = tmp.path() / "aaa-111.jsonl";
    auto f2 = tmp.path() / "bbb-222.jsonl";
    {
        std::ofstream(f1.string()) << R"({"type":"session_start","sessionId":"aaa-111","createdAt":"2026-07-30T10:00:00Z"})" << "\n";
    }
    // 确保两个文件的修改时间可区分（Windows 文件系统精度可能较低）
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    {
        std::ofstream(f2.string()) << R"({"type":"session_start","sessionId":"bbb-222","createdAt":"2026-07-31T10:00:00Z"})" << "\n";
    }

    auto sessions = SessionStore::list_sessions(tmp.string());
    REQUIRE(sessions.size() == 2);
    // 按修改时间倒序（f2 后创建，应在前）
    REQUIRE(sessions[0].session_id == "bbb-222");
    REQUIRE(sessions[1].session_id == "aaa-111");
}

TEST_CASE("session_store: list sessions empty dir", "[session][store]") {
    TempDir tmp("workx_test_empty_project");
    auto sessions = SessionStore::list_sessions(tmp.string());
    REQUIRE(sessions.empty());
}

TEST_CASE("session_store: list sessions non-existent dir", "[session][store]") {
    auto sessions = SessionStore::list_sessions("/nonexistent/path/12345");
    REQUIRE(sessions.empty());
}

// ============================================================
// load_messages
// ============================================================

TEST_CASE("session_store: load messages from jsonl", "[session][store]") {
    TempFile tmp("workx_test_load.jsonl");

    {
        std::ofstream f(tmp.string());
        f << R"({"type":"session_start","sessionId":"s1","cwd":"/tmp","model":"m","gitBranch":"main"})" << "\n";
        f << R"({"type":"user","uuid":"u1","parentUuid":"","timestamp":"t1","content":"hello"})" << "\n";
        f << R"({"type":"assistant","uuid":"a1","parentUuid":"u1","timestamp":"t2","content":"hi","reasoningContent":"thinking","toolUses":[]})" << "\n";
        f << R"({"type":"tool","uuid":"t1","parentUuid":"a1","timestamp":"t3","toolCallId":"tc1","toolName":"Read","content":"file content","isError":false})" << "\n";
        f << R"({"type":"session_end","sessionId":"s1"})" << "\n";
    }

    auto messages = SessionStore::load_messages(tmp.string());
    REQUIRE(messages.size() == 3);  // user + assistant + tool（不含 session_start/end）
    REQUIRE(messages[0].role == agent::ChatMessage::Role::User);
    REQUIRE(messages[0].content == "hello");
    REQUIRE(messages[1].role == agent::ChatMessage::Role::Assistant);
    REQUIRE(messages[1].content == "hi");
    REQUIRE(messages[1].reasoning_content == "thinking");
    REQUIRE(messages[2].role == agent::ChatMessage::Role::Tool);
    REQUIRE(messages[2].tool_call_id == "tc1");
    REQUIRE(messages[2].tool_name == "Read");
    REQUIRE(messages[2].content == "file content");
    REQUIRE_FALSE(messages[2].is_error);
}

TEST_CASE("session_store: load messages with tool_uses preserved", "[session][store]") {
    TempFile tmp("workx_test_load_uses.jsonl");

    {
        std::ofstream f(tmp.string());
        f << R"({"type":"assistant","uuid":"a1","parentUuid":"","timestamp":"t1","content":"call","reasoningContent":"","toolUses":[{"id":"toolu_1","name":"Read","input":{"path":"/tmp"}}]})" << "\n";
    }

    auto messages = SessionStore::load_messages(tmp.string());
    REQUIRE(messages.size() == 1);
    REQUIRE(messages[0].tool_uses.size() == 1);
    REQUIRE(messages[0].tool_uses[0].id == "toolu_1");
    REQUIRE(messages[0].tool_uses[0].name == "Read");
    REQUIRE(messages[0].tool_uses[0].input["path"] == "/tmp");
}

// ============================================================
// load_meta
// ============================================================

TEST_CASE("session_store: load meta from jsonl", "[session][store]") {
    TempFile tmp("workx_test_meta.jsonl");

    {
        std::ofstream f(tmp.string());
        f << R"({"type":"session_start","sessionId":"meta-1","cwd":"/project","model":"gpt-4","gitBranch":"develop"})" << "\n";
        f << R"({"type":"user","uuid":"u1","parentUuid":"","timestamp":"t1","content":"hi"})" << "\n";
        f << R"({"type":"assistant","uuid":"a1","parentUuid":"u1","timestamp":"t2","content":"","reasoningContent":"","toolUses":[]})" << "\n";
        f << R"({"type":"tool","uuid":"t1","parentUuid":"a1","timestamp":"t3","toolCallId":"c1","toolName":"Read","content":"","isError":false})" << "\n";
    }

    auto meta = SessionStore::load_meta(tmp.string());
    REQUIRE(meta.has_value());
    REQUIRE(meta->session_id == "meta-1");
    REQUIRE(meta->cwd == "/project");
    REQUIRE(meta->model == "gpt-4");
    REQUIRE(meta->git_branch == "develop");
    REQUIRE(meta->message_count == 3);  // user + assistant + tool
}

TEST_CASE("session_store: load meta fallback to filename", "[session][store]") {
    // 没有 session_start 事件时，用文件名作为 session_id
    TempFile tmp("fallback-id.jsonl");
    {
        std::ofstream f(tmp.string());
        f << R"({"type":"user","uuid":"u1","parentUuid":"","timestamp":"t1","content":"hi"})" << "\n";
    }

    auto meta = SessionStore::load_meta(tmp.string());
    REQUIRE(meta.has_value());
    REQUIRE(meta->session_id == "fallback-id");
    REQUIRE(meta->message_count == 1);
}

// ============================================================
// 损坏行容错
// ============================================================

TEST_CASE("session_store: skip corrupted lines", "[session][store]") {
    TempFile tmp("workx_test_corrupt.jsonl");
    {
        std::ofstream f(tmp.string());
        f << R"({"type":"user","uuid":"u1","parentUuid":"","timestamp":"t1","content":"good1"})" << "\n";
        f << "this is not json\n";
        f << R"({"type":"user","uuid":"u2","parentUuid":"","timestamp":"t2","content":"good2"})" << "\n";
        f << "{broken json\n";
        f << R"({"type":"user","uuid":"u3","parentUuid":"","timestamp":"t3","content":"good3"})" << "\n";
    }

    auto events = SessionStore::read_all(tmp.string());
    REQUIRE(events.size() == 3);  // 跳过 2 行损坏
    REQUIRE(events[0]["content"] == "good1");
    REQUIRE(events[1]["content"] == "good2");
    REQUIRE(events[2]["content"] == "good3");
}

// ============================================================
// get_project_session_dir
// ============================================================

TEST_CASE("session_store: get_project_session_dir", "[session][store]") {
    auto dir = get_project_session_dir("C:/Users/test/.workx", R"(D:\develop\workx)");
    std::string s = dir.string();
    // 应包含 projects 和编码后的路径
    REQUIRE(s.find("projects") != std::string::npos);
    REQUIRE(s.find("D--develop-workx") != std::string::npos);
}

// ============================================================
// title 事件
// ============================================================

TEST_CASE("session_store: append_title writes title event", "[session][store][title]") {
    TempFile tmp("workx_test_title_append.jsonl");

    {
        SessionStore store(tmp.string(), "title-session-1");
        REQUIRE(store.open());
        REQUIRE(store.append_session_start("/cwd", "model", "branch"));
        REQUIRE(store.append_title("我的会话标题"));
        store.close();
    }

    auto events = SessionStore::read_all(tmp.string());
    REQUIRE(events.size() == 2);
    REQUIRE(events[1]["type"] == "title");
    REQUIRE(events[1]["title"] == "我的会话标题");
    REQUIRE(events[1]["sessionId"] == "title-session-1");
}

TEST_CASE("session_store: load_meta reads title from last title event", "[session][store][title]") {
    TempFile tmp("workx_test_title_meta.jsonl");

    {
        std::ofstream f(tmp.string());
        f << R"({"type":"session_start","sessionId":"s1","cwd":"/p","model":"m","gitBranch":"b","createdAt":"2026-07-31T10:00:00Z"})" << "\n";
        f << R"({"type":"title","sessionId":"s1","timestamp":"t1","title":"旧标题"})" << "\n";
        f << R"({"type":"user","uuid":"u1","parentUuid":"","timestamp":"t2","content":"hello"})" << "\n";
        f << R"({"type":"title","sessionId":"s1","timestamp":"t3","title":"新标题"})" << "\n";
    }

    auto meta = SessionStore::load_meta(tmp.string());
    REQUIRE(meta.has_value());
    REQUIRE(meta->title == "新标题");  // 取最后一条 title 事件
}

TEST_CASE("session_store: load_meta title fallback to first user message 20 chars", "[session][store][title]") {
    TempFile tmp("workx_test_title_fallback.jsonl");

    {
        std::ofstream f(tmp.string());
        f << R"({"type":"session_start","sessionId":"s1","cwd":"/p","model":"m","gitBranch":"b","createdAt":"2026-07-31T10:00:00Z"})" << "\n";
        // 无 title 事件，应 fallback 到首条 user 消息前 20 字
        f << R"({"type":"user","uuid":"u1","parentUuid":"","timestamp":"t1","content":"这是一个比较长的用户消息用于测试标题截断功能是否正常工作"})" << "\n";
    }

    auto meta = SessionStore::load_meta(tmp.string());
    REQUIRE(meta.has_value());
    // 前 20 字 + "..."
    REQUIRE(meta->title.size() > 20);
    REQUIRE(meta->title.find("这是一个比较长的用户消息用于测试标题截断") != std::string::npos);
    REQUIRE(meta->title.back() == '.');  // 以 "..." 结尾
}

TEST_CASE("session_store: load_meta title fallback short message no ellipsis", "[session][store][title]") {
    TempFile tmp("workx_test_title_short.jsonl");

    {
        std::ofstream f(tmp.string());
        f << R"({"type":"user","uuid":"u1","parentUuid":"","timestamp":"t1","content":"短消息"})" << "\n";
    }

    auto meta = SessionStore::load_meta(tmp.string());
    REQUIRE(meta.has_value());
    REQUIRE(meta->title == "短消息");  // 不足 20 字，不加 "..."
}

TEST_CASE("session_store: load_meta title fallback no user message", "[session][store][title]") {
    TempFile tmp("workx_test_title_empty.jsonl");

    {
        std::ofstream f(tmp.string());
        f << R"({"type":"session_start","sessionId":"s1","cwd":"/p","model":"m","gitBranch":"b","createdAt":"2026-07-31T10:00:00Z"})" << "\n";
        // 无 title 事件，无 user 消息
    }

    auto meta = SessionStore::load_meta(tmp.string());
    REQUIRE(meta.has_value());
    REQUIRE(meta->title == "未命名会话");
}

TEST_CASE("session_store: title fallback UTF-8 safe truncation", "[session][store][title]") {
    TempFile tmp("workx_test_title_utf8.jsonl");

    {
        std::ofstream f(tmp.string());
        // 中文每字 3 字节 UTF-8，20 字 = 60 字节
        // 用 25 个中文字 = 75 字节，应截断到 20 字（60 字节）+ "..."
        std::string content(25, '\xE4');  // 无效 UTF-8，仅测试不崩溃
        // 改用真实中文字符
        content = "一二三四五六七八九十一二三四五六七八九十一二三四五";
        f << R"({"type":"user","uuid":"u1","parentUuid":"","timestamp":"t1","content":")" << content << R"("})" << "\n";
    }

    auto meta = SessionStore::load_meta(tmp.string());
    REQUIRE(meta.has_value());
    // 应截断到前 20 字 + "..."，不应截断多字节字符
    REQUIRE(meta->title.find("...") != std::string::npos);
}

TEST_CASE("session_store: multiple title events override", "[session][store][title]") {
    TempFile tmp("workx_test_title_multi.jsonl");

    {
        SessionStore store(tmp.string(), "multi-title");
        REQUIRE(store.open());
        store.append_session_start("/p", "m", "b");
        store.append_title("第一版");
        store.append_title("第二版");
        store.append_title("第三版");
        store.close();
    }

    auto meta = SessionStore::load_meta(tmp.string());
    REQUIRE(meta.has_value());
    REQUIRE(meta->title == "第三版");  // 最后一条生效
}

