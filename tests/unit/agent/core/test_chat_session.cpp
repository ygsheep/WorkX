/**
 * @file test_chat_session.cpp
 * @brief ChatSession 单元测试
 */

#include <catch2/catch_test_macros.hpp>
#include "agent/core/chat_session.h"
#include "core/events/event_bus.h"
#include "agent/message/types.h"
#include "core/config/config_manager.h"

using namespace agent;

namespace {

/// @brief Mock ICompletionProvider
class MockProvider : public ICompletionProvider {
public:
    int submit_count = 0;
    int interrupt_count = 0;

    std::unique_ptr<IStreamReader> submit_completion(const CompletionRequest& req) override {
        (void)req;
        submit_count++;
        return nullptr;
    }

    void interrupt() override {
        interrupt_count++;
    }

    bool is_generating() const override {
        return false;
    }
};

/// @brief 创建测试用 ChatSession
std::unique_ptr<ChatSession> make_test_session() {
    return std::make_unique<ChatSession>(
        std::unique_ptr<ICompletionProvider>(new MockProvider())
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

#ifdef WORKX_HAS_NLOHMANN_JSON
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
#endif
