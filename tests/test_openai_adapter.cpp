/**
 * @file test_openai_adapter.cpp
 * @brief OpenAIAdapter 单元测试
 */

#include <catch2/catch_test_macros.hpp>
#include "agent/api/provider/openai_adapter.h"

using namespace workx;

TEST_CASE("OpenAIAdapter build_url", "[provider][openai]") {
    OpenAIAdapter adapter;

    SECTION("normal base_url") {
        auto url = adapter.build_url("https://api.openai.com");
        REQUIRE(url == "https://api.openai.com/v1/chat/completions");
    }

    SECTION("base_url with trailing slash") {
        auto url = adapter.build_url("https://api.deepseek.com/");
        REQUIRE(url == "https://api.deepseek.com/v1/chat/completions");
    }
}

TEST_CASE("OpenAIAdapter build_headers", "[provider][openai]") {
    OpenAIAdapter adapter;

    SECTION("with API key") {
        auto headers = adapter.build_headers("sk-test123");
        REQUIRE(headers.size() >= 2);
        bool has_auth = false;
        for (const auto& [k, v] : headers) {
            if (k == "Authorization") {
                REQUIRE(v == "Bearer sk-test123");
                has_auth = true;
            }
        }
        REQUIRE(has_auth);
    }

    SECTION("without API key") {
        auto headers = adapter.build_headers("");
        bool has_auth = false;
        for (const auto& [k, v] : headers) {
            if (k == "Authorization") has_auth = true;
        }
        REQUIRE_FALSE(has_auth);
    }
}

TEST_CASE("OpenAIAdapter build_request_body", "[provider][openai]") {
    OpenAIAdapter adapter;

    CompletionRequest request;
    request.stream = true;
    request.messages.push_back(ChatMessage::system("You are helpful"));
    request.messages.push_back(ChatMessage::user("Hello"));
    request.messages.push_back(ChatMessage::assistant("Hi there!"));
    request.temperature = 0.7f;
    request.max_tokens = 100;

    auto body = adapter.build_request_body(request, "gpt-4o");

    // 验证 JSON 包含关键字段
    REQUIRE(body.find("\"model\":\"gpt-4o\"") != std::string::npos);
    REQUIRE(body.find("\"stream\":true") != std::string::npos);
    REQUIRE(body.find("\"temperature\":") != std::string::npos);
    REQUIRE(body.find("\"max_tokens\":") != std::string::npos);
    REQUIRE(body.find("\"role\":\"system\"") != std::string::npos);
    REQUIRE(body.find("\"role\":\"user\"") != std::string::npos);
    REQUIRE(body.find("\"role\":\"assistant\"") != std::string::npos);
}

TEST_CASE("OpenAIAdapter parse_sse_event", "[provider][openai]") {
    OpenAIAdapter adapter;

    SECTION("content delta") {
        StreamChunk chunk;
        bool result = adapter.parse_sse_event("",
            R"({"choices":[{"index":0,"delta":{"content":"Hello"},"finish_reason":null}]})",
            chunk);
        REQUIRE(result);
        REQUIRE(chunk.content_delta == "Hello");
        REQUIRE_FALSE(chunk.is_final);
    }

    SECTION("finish reason") {
        StreamChunk chunk;
        bool result = adapter.parse_sse_event("",
            R"({"choices":[{"index":0,"delta":{},"finish_reason":"stop"}],"usage":{"prompt_tokens":10,"completion_tokens":20}})",
            chunk);
        REQUIRE(result);
        REQUIRE(chunk.is_final);
    }

    SECTION("[DONE] signal") {
        StreamChunk chunk;
        bool result = adapter.parse_sse_event("", "[DONE]", chunk);
        REQUIRE(result);
        REQUIRE(chunk.is_final);
    }

    SECTION("reasoning content (deepseek format)") {
        StreamChunk chunk;
        bool result = adapter.parse_sse_event("",
            R"({"choices":[{"index":0,"delta":{"reasoning_content":"thinking..."},"finish_reason":null}]})",
            chunk);
        REQUIRE(result);
        REQUIRE(chunk.reasoning_delta == "thinking...");
    }

    SECTION("empty delta returns false") {
        StreamChunk chunk;
        bool result = adapter.parse_sse_event("",
            R"({"choices":[{"index":0,"delta":{},"finish_reason":null}]})",
            chunk);
        REQUIRE_FALSE(result);
    }

    SECTION("invalid JSON returns false") {
        StreamChunk chunk;
        bool result = adapter.parse_sse_event("", "not json", chunk);
        REQUIRE_FALSE(result);
    }
}
