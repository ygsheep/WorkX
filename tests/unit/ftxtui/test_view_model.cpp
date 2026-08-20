/**
 * @file test_view_model.cpp
 * @brief ViewModel 单元测试（ftxtui 无头逻辑）
 * @details 覆盖全部 15 个 Action 的 apply 语义：流式追加、滚动封口、工具块、
 *          错误/忙碌/权限/退出/提示，以及 active_stream 的复用/新建语义。
 */

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

#include "bridge/action.h"
#include "vm/view_model.h"

using namespace ftxtui;

// ============================================================================
// active_stream / has_active_stream
// ============================================================================

TEST_CASE("ViewModel active_stream creates a new assistant on empty", "[view_model][stream]") {
    ViewModel vm;
    REQUIRE_FALSE(vm.has_active_stream());
    auto& m = vm.active_stream();
    REQUIRE(m.role == MsgRole::Assistant);
    REQUIRE(vm.has_active_stream());
    REQUIRE(vm.messages.size() == 1);
}

TEST_CASE("ViewModel active_stream reuses an unsealed assistant", "[view_model][stream]") {
    ViewModel vm;
    auto& m1 = vm.active_stream();
    m1.text = "abc";
    auto& m2 = vm.active_stream();
    REQUIRE(&m1 == &m2);           // 复用同一节点
    REQUIRE(vm.messages.size() == 1);
}

TEST_CASE("ViewModel active_stream creates a new node after sealing", "[view_model][stream]") {
    ViewModel vm;
    REQUIRE(vm.apply(ActionTurnDone{.full_content = "done"}));  // 封口
    REQUIRE_FALSE(vm.has_active_stream());
    auto& next = vm.active_stream();
    REQUIRE(next.text.empty());
    REQUIRE(vm.messages.size() == 2);
}

TEST_CASE("ViewModel has_active_stream false after error message", "[view_model][stream]") {
    ViewModel vm;
    vm.apply(ActionError{.message = "boom"});
    REQUIRE_FALSE(vm.has_active_stream());
    REQUIRE(vm.messages.back().role == MsgRole::Error);
}

// ============================================================================
// ActionAppendMessage
// ============================================================================

TEST_CASE("ViewModel append user message marks user + sealed", "[view_model][append]") {
    ViewModel vm;
    REQUIRE(vm.apply(ActionAppendMessage{.role = "user", .text = "hi"}));
    REQUIRE(vm.messages.size() == 1);
    REQUIRE(vm.messages[0].role == MsgRole::User);
    REQUIRE(vm.messages[0].sealed);
    REQUIRE(vm.messages[0].text == "hi");
}

TEST_CASE("ViewModel append assistant message marks assistant + sealed", "[view_model][append]") {
    ViewModel vm;
    vm.apply(ActionAppendMessage{.role = "assistant", .text = "reply"});
    REQUIRE(vm.messages[0].role == MsgRole::Assistant);
    REQUIRE(vm.messages[0].sealed);
}

// ============================================================================
// 流式增量
// ============================================================================

TEST_CASE("ViewModel token delta appends and flips streaming", "[view_model][token]") {
    ViewModel vm;
    REQUIRE(vm.apply(ActionTokenDelta{.content_delta = "Hello "}));
    REQUIRE(vm.apply(ActionTokenDelta{.content_delta = "world"}));
    REQUIRE(vm.active_stream().text == "Hello world");
    REQUIRE(vm.active_stream().streaming);
}

TEST_CASE("ViewModel reasoning delta marks reasoned and expands by default", "[view_model][reasoning]") {
    ViewModel vm;
    vm.apply(ActionReasoningDelta{.delta = "think"});
    auto& m = vm.messages.back();
    REQUIRE(m.reasoned);
    REQUIRE(m.reasoning_expanded == vm.card_defaults.reasoning_expanded);
    REQUIRE(m.reasoning == "think");
}

TEST_CASE("ViewModel reasoning empty delta does not mark reasoned", "[view_model][reasoning]") {
    ViewModel vm;
    vm.apply(ActionReasoningDelta{.delta = ""});
    REQUIRE_FALSE(vm.messages.back().reasoned);
}

TEST_CASE("ViewModel step done clears streaming on back assistant", "[view_model][step_done]") {
    ViewModel vm;
    vm.apply(ActionTokenDelta{.content_delta = "x"});
    REQUIRE(vm.active_stream().streaming);
    vm.apply(ActionStepDone{});
    REQUIRE_FALSE(vm.active_stream().streaming);
}

// ============================================================================
// ActionTurnDone
// ============================================================================

TEST_CASE("ViewModel turn done fills text when empty and seals", "[view_model][turn_done]") {
    ViewModel vm;
    vm.apply(ActionTurnDone{.full_content = "final", .prompt_tokens = 10,
                            .generated_tokens = 20, .cache_read_input_tokens = 5,
                            .prompt_ms = 100.0, .generation_ms = 200.0,
                            .reasoning_ms = 100.0});
    auto& m = vm.messages.back();
    REQUIRE(m.text == "final");
    REQUIRE(m.sealed);
    REQUIRE_FALSE(m.streaming);
    REQUIRE(m.prompt_tokens == 10);
    REQUIRE(m.generated_tokens == 20);
    REQUIRE(m.cache_read_tokens == 5);
    REQUIRE(m.duration_ms == 300.0);  // 100+200 精确可表示
    REQUIRE(m.reasoning_ms == 100.0);
    REQUIRE(vm.busy == false);
    REQUIRE(vm.total_tokens == 35);
    REQUIRE(vm.sidebar.context_used == 10);
}

TEST_CASE("ViewModel turn done keeps existing streamed text", "[view_model][turn_done]") {
    ViewModel vm;
    vm.apply(ActionTokenDelta{.content_delta = "streamed"});
    vm.apply(ActionTurnDone{.full_content = "final"});
    REQUIRE(vm.messages.back().text == "streamed");
}

// ============================================================================
// 工具调用
// ============================================================================

TEST_CASE("ViewModel begin tool creates running block", "[view_model][tool]") {
    ViewModel vm;
    REQUIRE(vm.apply(ActionBeginTool{.tool_name = "Read", .call_id = "c1",
                                     .arguments = "{}"}));
    auto& t = vm.messages.back().tool_calls[0];
    REQUIRE(t.tool_name == "Read");
    REQUIRE(t.call_id == "c1");
    REQUIRE(t.running);
    REQUIRE(t.expanded == vm.card_defaults.tool_expanded);
    REQUIRE(vm.messages.back().tool_use_ids.size() == 1);
}

TEST_CASE("ViewModel begin tool deduplicates same call_id", "[view_model][tool]") {
    ViewModel vm;
    vm.apply(ActionBeginTool{.tool_name = "Read", .call_id = "c1"});
    REQUIRE_FALSE(vm.apply(ActionBeginTool{.tool_name = "Read", .call_id = "c1"}));  // 重复
    REQUIRE(vm.messages.back().tool_calls.size() == 1);
}

TEST_CASE("ViewModel end tool marks done and records result", "[view_model][tool]") {
    ViewModel vm;
    vm.apply(ActionBeginTool{.tool_name = "Read", .call_id = "c1"});
    REQUIRE(vm.apply(ActionEndTool{.call_id = "c1", .result = "content"}));
    auto& t = vm.messages.back().tool_calls[0];
    REQUIRE(t.done);
    REQUIRE_FALSE(t.running);
    REQUIRE(t.result == "content");
    REQUIRE_FALSE(t.is_error);
    REQUIRE_FALSE(t.expanded);  // 成功默认收起
}

TEST_CASE("ViewModel end tool error expands by default", "[view_model][tool]") {
    ViewModel vm;
    vm.apply(ActionBeginTool{.tool_name = "Bash", .call_id = "c1"});
    vm.apply(ActionEndTool{.call_id = "c1", .result = "err", .is_error = true});
    auto& t = vm.messages.back().tool_calls[0];
    REQUIRE(t.is_error);
    REQUIRE(t.expanded);
}

TEST_CASE("ViewModel end tool unknown call_id returns false", "[view_model][tool]") {
    ViewModel vm;
    REQUIRE_FALSE(vm.apply(ActionEndTool{.call_id = "missing", .result = ""}));
}

// ============================================================================
// ActionAgentDone
// ============================================================================

TEST_CASE("ViewModel agent done seals unsealed assistant and fills text", "[view_model][agent_done]") {
    ViewModel vm;
    vm.apply(ActionSetBusy{.busy = true});  // 预留 assistant
    vm.apply(ActionAgentDone{.final_answer = "answer", .total_duration_ms = 99.0});
    auto& m = vm.messages.back();
    REQUIRE(m.sealed);
    REQUIRE(m.text == "answer");
    REQUIRE(m.duration_ms == 99.0);
    REQUIRE_FALSE(vm.busy);
}

TEST_CASE("ViewModel agent done does not append fallback message when last is sealed", "[view_model][agent_done]") {
    ViewModel vm;
    // 正常流式完成路径：StreamDoneEvent 已封口（ActionTurnDone），
    // AgentDoneEvent 只是最终汇总，不得追加第二条（否则回复显示两遍）
    vm.apply(ActionAppendMessage{.role = "user", .text = "q"});
    vm.apply(ActionSetBusy{.busy = true});  // 预留 assistant
    vm.apply(ActionTurnDone{.full_content = "answer", .prompt_ms = 10.0, .generation_ms = 20.0});
    REQUIRE(vm.messages.back().sealed);
    vm.apply(ActionAgentDone{.final_answer = "answer", .total_duration_ms = 99.0});
    REQUIRE(vm.messages.size() == 2);  // user + assistant，无第三遍
    REQUIRE(vm.messages.back().role == MsgRole::Assistant);
    REQUIRE(vm.messages.back().text == "answer");
    REQUIRE_FALSE(vm.busy);
}

TEST_CASE("ViewModel agent done fills unsealed assistant without active stream", "[view_model][agent_done]") {
    ViewModel vm;
    // 流式事件缺失（异常/丢失）：未封口 assistant 由 AgentDone 补填封口
    vm.apply(ActionAppendMessage{.role = "user", .text = "q"});
    vm.apply(ActionSetBusy{.busy = true});
    vm.apply(ActionAgentDone{.final_answer = "answer", .total_duration_ms = 99.0});
    REQUIRE(vm.messages.size() == 2);
    REQUIRE(vm.messages.back().role == MsgRole::Assistant);
    REQUIRE(vm.messages.back().text == "answer");
    REQUIRE(vm.messages.back().sealed);
    REQUIRE(vm.messages.back().duration_ms == 99.0);
    REQUIRE_FALSE(vm.busy);
}

TEST_CASE("ViewModel agent done with empty answer does nothing", "[view_model][agent_done]") {
    ViewModel vm;
    vm.apply(ActionAgentDone{});
    REQUIRE(vm.messages.empty());
}

// ============================================================================
// ActionSetBusy
// ============================================================================

TEST_CASE("ViewModel set busy reserves an assistant node", "[view_model][busy]") {
    ViewModel vm;
    REQUIRE(vm.apply(ActionSetBusy{.busy = true}));
    REQUIRE(vm.busy);
    REQUIRE_FALSE(vm.messages.empty());
    REQUIRE_FALSE(vm.messages.back().streaming);
    vm.apply(ActionSetBusy{.busy = false});
    REQUIRE_FALSE(vm.busy);
}

// ============================================================================
// 杂项
// ============================================================================

TEST_CASE("ViewModel permissions updates and deduplicates", "[view_model][misc]") {
    ViewModel vm;
    REQUIRE(vm.apply(ActionPermissions{.label = "plan"}));
    REQUIRE(vm.sidebar.permission == "plan");
    REQUIRE_FALSE(vm.apply(ActionPermissions{.label = "plan"}));  // 相同不变化
    REQUIRE(vm.apply(ActionPermissions{.label = "bypass"}));
    REQUIRE(vm.sidebar.permission == "bypass");
}

TEST_CASE("ViewModel shutdown raises pending_exit", "[view_model][misc]") {
    ViewModel vm;
    REQUIRE(vm.apply(ActionShutdown{}));
    REQUIRE(vm.pending_exit);
}

TEST_CASE("ViewModel toast stores prompt echo", "[view_model][misc]") {
    ViewModel vm;
    REQUIRE(vm.apply(ActionToast{.text = "command output"}));
    REQUIRE(vm.prompt_echo == "command output");
}

TEST_CASE("ViewModel models loaded is ignored by view model", "[view_model][misc]") {
    ViewModel vm;
    REQUIRE_FALSE(vm.apply(ActionModelsLoaded{.models = {"m1"}}));
}

// ============================================================================
// 综合：消息历史组装
// ============================================================================

TEST_CASE("ViewModel assembles a full multi-turn history", "[view_model][integration]") {
    ViewModel vm;
    vm.apply(ActionAppendMessage{.role = "user", .text = "Q1"});
    vm.apply(ActionSetBusy{.busy = true});
    vm.apply(ActionReasoningDelta{.delta = "reason"});
    vm.apply(ActionTokenDelta{.content_delta = "A1"});
    vm.apply(ActionAgentDone{.final_answer = "A1", .total_tool_calls = 0});
    vm.apply(ActionAppendMessage{.role = "user", .text = "Q2"});

    REQUIRE(vm.messages.size() == 3);
    REQUIRE(vm.messages[0].role == MsgRole::User);
    REQUIRE(vm.messages[0].text == "Q1");
    REQUIRE(vm.messages[1].role == MsgRole::Assistant);
    REQUIRE(vm.messages[1].reasoned);
    REQUIRE(vm.messages[1].text == "A1");
    REQUIRE(vm.messages[1].sealed);
    REQUIRE(vm.messages[2].role == MsgRole::User);
    REQUIRE(vm.messages[2].text == "Q2");
}

// ============================================================================
// 子 Agent 聚合（任务调度 tab）
// ============================================================================

TEST_CASE("ViewModel sub agent progress aggregates by task_id", "[view_model][subagent]") {
    ViewModel vm;
    vm.apply(ActionSubAgentProgress{.task_id = "t1", .step_number = 1, .step_type = "thought"});
    REQUIRE(vm.tabs.sub_agents.size() == 1);
    REQUIRE(vm.tabs.sub_agents[0].task_id == "t1");
    REQUIRE(vm.tabs.sub_agents[0].status == "running");
    REQUIRE(vm.tabs.sub_agents[0].step_number == 1);
    REQUIRE(vm.tabs.sub_agents[0].msg_index == 0);  // 指向刚 push 的消息

    // 同一 task_id 再次进度：不新增条目，更新步骤
    vm.apply(ActionSubAgentProgress{.task_id = "t1", .step_number = 2, .step_type = "action"});
    REQUIRE(vm.tabs.sub_agents.size() == 1);
    REQUIRE(vm.tabs.sub_agents[0].step_number == 2);
    REQUIRE(vm.tabs.sub_agents[0].msg_index == 1);

    // 新 task_id：新增条目
    vm.apply(ActionSubAgentProgress{.task_id = "t2", .step_number = 1, .step_type = "thought"});
    REQUIRE(vm.tabs.sub_agents.size() == 2);
    REQUIRE(vm.tabs.sub_agents[1].task_id == "t2");
    REQUIRE(vm.tabs.sub_agents[1].msg_index == 2);
    REQUIRE(vm.messages.size() == 3);
}

TEST_CASE("ViewModel sub agent final step does not append message", "[view_model][subagent]") {
    ViewModel vm;
    vm.apply(ActionSubAgentProgress{.task_id = "t1", .step_number = 1, .step_type = "final"});
    REQUIRE(vm.messages.empty());
    REQUIRE(vm.tabs.sub_agents.empty());  // final 步不聚合，由 Completed 处理
}

TEST_CASE("ViewModel sub agent completed updates status and msg_index", "[view_model][subagent]") {
    ViewModel vm;
    vm.apply(ActionSubAgentProgress{.task_id = "t1", .step_number = 1, .step_type = "thought"});
    vm.apply(ActionSubAgentCompleted{.task_id = "t1", .final_answer = "done", .was_error = false,
                                     .duration_ms = 1500.0});
    REQUIRE(vm.tabs.sub_agents.size() == 1);
    REQUIRE(vm.tabs.sub_agents[0].status == "done");
    REQUIRE(vm.tabs.sub_agents[0].duration_ms == 1500.0);
    REQUIRE(vm.tabs.sub_agents[0].msg_index == 1);  // 指向完成消息

    // 失败：状态为 failed
    vm.apply(ActionSubAgentCompleted{.task_id = "t2", .was_error = true, .duration_ms = 500.0});
    REQUIRE(vm.tabs.sub_agents.size() == 2);
    REQUIRE(vm.tabs.sub_agents[1].status == "failed");
    REQUIRE(vm.tabs.sub_agents[1].msg_index == 2);
}

// ============================================================================
// 修改追踪（P4：Edit/Write → FileChange）
// ============================================================================

TEST_CASE("ViewModel edit tool tracks FileChange with diff", "[view_model][file_change]") {
    ViewModel vm;
    vm.apply(ActionReasoningDelta{.delta = "改用 bar() 计算 y\n"});
    vm.apply(ActionBeginTool{
        .tool_name = "Edit",
        .call_id = "c1",
        .arguments = R"JSON({"file_path": "src/main.cpp",
                             "old_string": "auto y = foo();",
                             "new_string": "auto y = bar();"})JSON"});

    REQUIRE(vm.tabs.changes.changes.size() == 1);
    const auto& ch = vm.tabs.changes.changes[0];
    REQUIRE(ch.file_path == "src/main.cpp");
    REQUIRE(ch.old_string == "auto y = foo();");
    REQUIRE(ch.new_string == "auto y = bar();");
    REQUIRE(ch.purpose == "改用 bar() 计算 y");  // reasoning 最后一行
    REQUIRE(ch.msg_index == 0);                  // 工具调用所在消息
    REQUIRE(ch.timestamp > 0);
    REQUIRE(ch.diff.size() == 1);
    REQUIRE(ch.diff[0].kind == agent::DiffKind::Modify);
    REQUIRE(ch.diff[0].text == "auto y = bar();");
    REQUIRE(ch.diff[0].line_no == 1);
    REQUIRE(vm.tabs.changes_open);               // 首个修改自动打开变更记录 tab
}

TEST_CASE("ViewModel write tool tracks FileChange as all Insert", "[view_model][file_change]") {
    ViewModel vm;
    vm.apply(ActionBeginTool{
        .tool_name = "Write",
        .call_id = "c1",
        .arguments = R"JSON({"file_path": "src/new.cpp",
                             "content": "int main() {\n    return 0;\n}\n"})JSON"});

    REQUIRE(vm.tabs.changes.changes.size() == 1);
    const auto& ch = vm.tabs.changes.changes[0];
    REQUIRE(ch.old_string.empty());              // Write 全量改写，无旧内容
    REQUIRE(ch.new_string == "int main() {\n    return 0;\n}\n");
    REQUIRE(ch.diff.size() == 3);
    REQUIRE(ch.diff[0].kind == agent::DiffKind::Insert);
    REQUIRE(ch.diff[1].kind == agent::DiffKind::Insert);
    REQUIRE(ch.diff[2].kind == agent::DiffKind::Insert);
    REQUIRE(ch.diff[2].line_no == 3);
}

TEST_CASE("ViewModel purpose falls back to new_string first line", "[view_model][file_change]") {
    ViewModel vm;
    // 无 reasoning 直接调工具（R1 回退）
    vm.apply(ActionBeginTool{
        .tool_name = "Edit",
        .call_id = "c1",
        .arguments = R"JSON({"file_path": "a.txt",
                             "old_string": "x",
                             "new_string": "y"})JSON"});
    REQUIRE(vm.tabs.changes.changes.size() == 1);
    REQUIRE(vm.tabs.changes.changes[0].purpose == "y");
}

TEST_CASE("ViewModel non-file tools do not track FileChange", "[view_model][file_change]") {
    ViewModel vm;
    vm.apply(ActionBeginTool{.tool_name = "Read", .call_id = "c1",
                             .arguments = R"JSON({"file_path": "a.txt"})JSON"});
    vm.apply(ActionBeginTool{.tool_name = "Bash", .call_id = "c2",
                             .arguments = R"JSON({"command": "ls"})JSON"});
    REQUIRE(vm.tabs.changes.changes.empty());
    REQUIRE_FALSE(vm.tabs.changes_open);
}

TEST_CASE("ViewModel malformed arguments do not track FileChange", "[view_model][file_change]") {
    ViewModel vm;
    vm.apply(ActionBeginTool{.tool_name = "Edit", .call_id = "c1",
                             .arguments = "not json"});
    vm.apply(ActionBeginTool{.tool_name = "Edit", .call_id = "c2",
                             .arguments = R"JSON({"old_string": "x"})JSON"});  // 缺 file_path
    REQUIRE(vm.tabs.changes.changes.empty());
}

TEST_CASE("ViewModel multiple edits accumulate FileChanges", "[view_model][file_change]") {
    ViewModel vm;
    vm.apply(ActionBeginTool{
        .tool_name = "Edit", .call_id = "c1",
        .arguments = R"JSON({"file_path": "a.txt", "old_string": "1", "new_string": "2"})JSON"});
    vm.apply(ActionBeginTool{
        .tool_name = "Edit", .call_id = "c2",
        .arguments = R"JSON({"file_path": "b.txt", "old_string": "3", "new_string": "4"})JSON"});
    REQUIRE(vm.tabs.changes.changes.size() == 2);
    REQUIRE(vm.tabs.changes.changes[0].file_path == "a.txt");
    REQUIRE(vm.tabs.changes.changes[1].file_path == "b.txt");
}