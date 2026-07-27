/**
 * @file test_chat_session.cpp
 * @brief ChatSession 单元测试
 */

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include "agent/core/chat_session.h"
#include "core/events/event_bus.h"
#include "core/task/task_manager.h"
#include "agent/message/types.h"
#include "agent/api/retry.h"
#include "core/config/config_manager.h"
#include "helpers/mock_provider.h"

using namespace agent;

namespace {

/// @brief 创建测试用 ChatSession（使用共享 MockCompletionProvider）
/// @details H-4：DI 三件套必须显式注入，使用单例满足测试需求
std::unique_ptr<ChatSession> make_test_session() {
    return std::make_unique<ChatSession>(
        std::unique_ptr<ICompletionProvider>(new test::MockCompletionProvider()),
        TaskManager::instance(),
        EventBus::instance(),
        ConfigManager::instance()
    );
}

} // anonymous namespace

TEST_CASE("ChatSession basic operations", "[session]") {
    auto& cfg = ConfigManager::instance();
    cfg.clear_for_test();
    cfg.set("backend.retry_count", 0);

    auto session = make_test_session();

    REQUIRE_FALSE(session->is_generating());
    REQUIRE(session->get_messages().empty());

    session->set_system_prompt("You are helpful");
    session->clear_history();

    cfg.clear_for_test();
}

TEST_CASE("ChatSession load non-existent file", "[session]") {
    auto& cfg = ConfigManager::instance();
    cfg.clear_for_test();
    cfg.set("backend.retry_count", 0);

    auto session = make_test_session();

    auto load_result = session->load_session("nonexistent_file_12345.json");
    REQUIRE(load_result.isErr());

    cfg.clear_for_test();
}

TEST_CASE("ChatSession save and load round-trip", "[session]") {
    auto& cfg = ConfigManager::instance();
    cfg.clear_for_test();
    cfg.set("backend.retry_count", 0);

    std::string test_path = "test_roundtrip_tmp.json";

    {
        auto session = make_test_session();
        session->set_system_prompt("Test system prompt");

        auto save_result = session->save_session(test_path);
        REQUIRE(save_result.isOk());
    }

    {
        auto session = make_test_session();

        auto load_result = session->load_session(test_path);
        REQUIRE(load_result.isOk());
    }

    std::filesystem::remove(test_path);
    cfg.clear_for_test();
}

// ============================================================
// H-6: serialize_state / deserialize_state 纯函数测试
// ============================================================

TEST_CASE("ChatSession serialize_state empty messages", "[session][h6][serialize]") {
    auto& cfg = ConfigManager::instance();
    cfg.clear_for_test();
    cfg.set("backend.retry_count", 0);

    auto session = make_test_session();
    // 不设置 system_prompt，不添加 messages
    auto j = session->serialize_state();

    // 空 system_prompt 不应出现在 JSON 中
    REQUIRE_FALSE(j.contains("system_prompt"));
    // messages 应为空数组
    REQUIRE(j.contains("messages"));
    REQUIRE(j["messages"].is_array());
    REQUIRE(j["messages"].empty());

    cfg.clear_for_test();
}

TEST_CASE("ChatSession serialize_state with system_prompt", "[session][h6][serialize]") {
    auto& cfg = ConfigManager::instance();
    cfg.clear_for_test();
    cfg.set("backend.retry_count", 0);

    auto session = make_test_session();
    session->set_system_prompt("You are helpful");

    auto j = session->serialize_state();
    REQUIRE(j["system_prompt"] == "You are helpful");

    cfg.clear_for_test();
}

TEST_CASE("ChatSession serialize_state all roles", "[session][h6][serialize]") {
    auto& cfg = ConfigManager::instance();
    cfg.clear_for_test();
    cfg.set("backend.retry_count", 0);

    auto session = make_test_session();
    // 通过 save/load 路径不便构造消息，直接走 serialize_state 反向覆盖
    // 先设置一组消息
    session->set_system_prompt("sys");
    {
        // 直接通过 deserialize 注入消息以测试 serialize
        nlohmann::json input;
        input["system_prompt"] = "sys";
        input["messages"] = nlohmann::json::array({
            {{"role", "system"}, {"content", "sys msg"}},
            {{"role", "user"}, {"content", "hello"}},
            {{"role", "assistant"}, {"content", "hi"}, {"reasoning_content", "thinking"}},
            {{"role", "tool"}, {"content", "result"}, {"tool_call_id", "tc1"}, {"tool_name", "Read"}}
        });
        auto r = session->deserialize_state(input);
        REQUIRE(r.isOk());
    }

    auto j = session->serialize_state();
    REQUIRE(j["system_prompt"] == "sys");
    REQUIRE(j["messages"].size() == 4);

    // 验证字段顺序与内容
    REQUIRE(j["messages"][0]["role"] == "system");
    REQUIRE(j["messages"][0]["content"] == "sys msg");

    REQUIRE(j["messages"][1]["role"] == "user");
    REQUIRE(j["messages"][1]["content"] == "hello");

    REQUIRE(j["messages"][2]["role"] == "assistant");
    REQUIRE(j["messages"][2]["content"] == "hi");
    REQUIRE(j["messages"][2]["reasoning_content"] == "thinking");

    REQUIRE(j["messages"][3]["role"] == "tool");
    REQUIRE(j["messages"][3]["content"] == "result");
    REQUIRE(j["messages"][3]["tool_call_id"] == "tc1");
    REQUIRE(j["messages"][3]["tool_name"] == "Read");

    cfg.clear_for_test();
}

TEST_CASE("ChatSession deserialize_state round-trip", "[session][h6][deserialize]") {
    auto& cfg = ConfigManager::instance();
    cfg.clear_for_test();
    cfg.set("backend.retry_count", 0);

    auto session1 = make_test_session();
    session1->set_system_prompt("round-trip test");

    // 通过 deserialize 注入消息
    nlohmann::json input;
    input["system_prompt"] = "round-trip test";
    input["messages"] = nlohmann::json::array({
        {{"role", "user"}, {"content", "q1"}},
        {{"role", "assistant"}, {"content", "a1"}}
    });
    REQUIRE(session1->deserialize_state(input).isOk());

    // serialize → deserialize round-trip
    auto j = session1->serialize_state();

    auto session2 = make_test_session();
    REQUIRE(session2->deserialize_state(j).isOk());

    auto j2 = session2->serialize_state();
    REQUIRE(j == j2);

    cfg.clear_for_test();
}

TEST_CASE("ChatSession deserialize_state rejects unknown role", "[session][h6][deserialize]") {
    auto& cfg = ConfigManager::instance();
    cfg.clear_for_test();
    cfg.set("backend.retry_count", 0);

    auto session = make_test_session();
    nlohmann::json input;
    input["messages"] = nlohmann::json::array({
        {{"role", "alien"}, {"content", "??"}}
    });

    auto r = session->deserialize_state(input);
    REQUIRE(r.isErr());
    REQUIRE(r.error().find("Unknown role") != std::string::npos);

    cfg.clear_for_test();
}

TEST_CASE("ChatSession deserialize_state accepts missing messages", "[session][h6][deserialize]") {
    auto& cfg = ConfigManager::instance();
    cfg.clear_for_test();
    cfg.set("backend.retry_count", 0);

    auto session = make_test_session();
    nlohmann::json input;  // 完全空对象
    auto r = session->deserialize_state(input);
    REQUIRE(r.isOk());
    REQUIRE(session->get_messages().empty());

    cfg.clear_for_test();
}

TEST_CASE("ChatSession deserialize_state rejects malformed content", "[session][h6][deserialize]") {
    auto& cfg = ConfigManager::instance();
    cfg.clear_for_test();
    cfg.set("backend.retry_count", 0);

    auto session = make_test_session();
    // content 字段缺失
    nlohmann::json input;
    input["messages"] = nlohmann::json::array({
        nlohmann::json::object({{"role", "user"}})  // 缺 content
    });

    auto r = session->deserialize_state(input);
    REQUIRE(r.isErr());

    cfg.clear_for_test();
}

// ============================================================
// H-7: compute_retry 纯函数测试
// ============================================================

TEST_CASE("compute_retry returns Continue when no error", "[session][h7][retry]") {
    ReActResult r;
    r.was_error = false;
    HttpRetryPolicy policy{.max_retries = 3, .base_delay_ms = 1000};

    auto d = ChatSession::compute_retry(r, policy, 0);
    REQUIRE(d.action == RetryAction::Continue);
    REQUIRE(d.delay_ms == 0);
}

TEST_CASE("compute_retry returns Sleep when retryable", "[session][h7][retry]") {
    ReActResult r;
    r.was_error = true;
    r.error_message = "connection timeout";  // 网络错误，可重试
    HttpRetryPolicy policy{.max_retries = 3, .base_delay_ms = 1000};

    // attempt=0: delay = 1000 * 2^0 = 1000
    auto d0 = ChatSession::compute_retry(r, policy, 0);
    REQUIRE(d0.action == RetryAction::Sleep);
    REQUIRE(d0.delay_ms == 1000);

    // attempt=1: delay = 1000 * 2^1 = 2000
    auto d1 = ChatSession::compute_retry(r, policy, 1);
    REQUIRE(d1.action == RetryAction::Sleep);
    REQUIRE(d1.delay_ms == 2000);

    // attempt=2: delay = 1000 * 2^2 = 4000
    auto d2 = ChatSession::compute_retry(r, policy, 2);
    REQUIRE(d2.action == RetryAction::Sleep);
    REQUIRE(d2.delay_ms == 4000);
}

TEST_CASE("compute_retry returns Stop when attempts exhausted", "[session][h7][retry]") {
    ReActResult r;
    r.was_error = true;
    r.error_message = "connection timeout";
    HttpRetryPolicy policy{.max_retries = 3, .base_delay_ms = 1000};

    // attempt == max_retries: 不可重试
    auto d = ChatSession::compute_retry(r, policy, 3);
    REQUIRE(d.action == RetryAction::Stop);
    REQUIRE(d.delay_ms == 0);

    // attempt > max_retries: 同样不可重试
    auto d2 = ChatSession::compute_retry(r, policy, 5);
    REQUIRE(d2.action == RetryAction::Stop);
}

TEST_CASE("compute_retry returns Stop for non-retryable error", "[session][h7][retry]") {
    ReActResult r;
    r.was_error = true;
    r.error_message = "max iterations reached";  // 业务错误，不可重试
    HttpRetryPolicy policy{.max_retries = 3, .base_delay_ms = 1000};

    auto d = ChatSession::compute_retry(r, policy, 0);
    REQUIRE(d.action == RetryAction::Stop);
    REQUIRE(d.delay_ms == 0);
}

TEST_CASE("compute_retry respects max_delay_ms cap", "[session][h7][retry]") {
    ReActResult r;
    r.was_error = true;
    r.error_message = "503 service unavailable";
    HttpRetryPolicy policy{.max_retries = 30, .base_delay_ms = 1000, .max_delay_ms = 60000};

    // attempt=10: 1000 * 2^10 = 102400 > 60000 → 截断到 60000
    auto d = ChatSession::compute_retry(r, policy, 10);
    REQUIRE(d.action == RetryAction::Sleep);
    REQUIRE(d.delay_ms == 60000);
}

TEST_CASE("compute_retry handles empty error_message", "[session][h7][retry]") {
    ReActResult r;
    r.was_error = true;
    r.error_message = "";  // 空 message：http_status=0 且 msg 为空 → 不可重试
    HttpRetryPolicy policy{.max_retries = 3, .base_delay_ms = 1000};

    auto d = ChatSession::compute_retry(r, policy, 0);
    REQUIRE(d.action == RetryAction::Stop);
}

// ============================================================
// H-8: 验证 ChatSession 不再暴露 backend()
// ============================================================

// 注：H-8 删除了 ChatSession::backend() 方法。若代码回退恢复该方法，
// 以下编译期 static_assert 会立即失败（&ChatSession::backend 在成员不存在时
// 是 ill-formed，编译报错）。这是预期的——H-8 的"测试"主要体现在
// SessionResult.backend_admin 字段的填充与使用上（见 test_factory.cpp）。
// 此处仅保留文档性注释，无 runtime 测试用例。
