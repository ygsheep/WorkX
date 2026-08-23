/**
 * @file test_todo_item.cpp
 * @brief #24：TodoItem 状态转换 + JSON 序列化往返
 * @date 2026-08
 */

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "core/todo/todo_item.h"

using namespace core::todo;

TEST_CASE("TodoItem status_str maps enum to string", "[todo][item]") {
    REQUIRE(std::string(TodoItem::status_str(TodoStatus::Pending)) == "pending");
    REQUIRE(std::string(TodoItem::status_str(TodoStatus::InProgress)) == "in_progress");
    REQUIRE(std::string(TodoItem::status_str(TodoStatus::Completed)) == "completed");
}

TEST_CASE("TodoItem status_from maps string to enum", "[todo][item]") {
    REQUIRE(TodoItem::status_from("pending") == TodoStatus::Pending);
    REQUIRE(TodoItem::status_from("in_progress") == TodoStatus::InProgress);
    REQUIRE(TodoItem::status_from("completed") == TodoStatus::Completed);
    // 非法输入回退 Pending
    REQUIRE(TodoItem::status_from("bogus") == TodoStatus::Pending);
    REQUIRE(TodoItem::status_from("") == TodoStatus::Pending);
}

TEST_CASE("TodoItem JSON roundtrip preserves all fields", "[todo][item]") {
    TodoItem item;
    item.id = "3";
    item.content = "Run tests";
    item.active_form = "Running tests";
    item.status = TodoStatus::InProgress;
    item.description = "Run the full suite";
    item.owner = "alice";
    item.blocks = {"5"};
    item.blocked_by = {"2"};
    item.metadata = {{"priority", "high"}};

    nlohmann::json j = item;
    TodoItem back = j.get<TodoItem>();

    REQUIRE(back.id == item.id);
    REQUIRE(back.content == item.content);
    REQUIRE(back.active_form == item.active_form);
    REQUIRE(back.status == item.status);
    REQUIRE(back.description == item.description);
    REQUIRE(back.owner == item.owner);
    REQUIRE(back.blocks == item.blocks);
    REQUIRE(back.blocked_by == item.blocked_by);
    REQUIRE(back.metadata["priority"] == "high");
    REQUIRE(back == item);
}

TEST_CASE("TodoItem from_json fills defaults for missing fields", "[todo][item]") {
    nlohmann::json j = {
        {"content", "Fix bug"},
    };
    TodoItem item = j.get<TodoItem>();
    REQUIRE(item.content == "Fix bug");
    REQUIRE(item.id.empty());
    REQUIRE(item.status == TodoStatus::Pending);
    REQUIRE(item.active_form.empty());
    REQUIRE(item.metadata.is_object());
    REQUIRE(item.metadata.empty());
}
