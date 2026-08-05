/**
 * @file test_command_panel.cpp
 * @brief 命令面板单元测试
 * @details set_commands/set_filter/move 回绕/大列表滚动窗口/get_completion 边界
 * @note render/clear 依赖未 initialize() 的 Terminal（m_platform 为 nullptr），
 *       渲染坐标路径不可在无终端环境验证；本测试只覆盖状态逻辑：
 *       过滤/导航回绕/大列表滚动窗口/get_completion 边界。坐标不越界
 *       由 render 公式（底行固定 h-3）+ overlay 范围（[h-12, h-3]）一致性保证。
 */

#include <catch2/catch_test_macros.hpp>

#include "tui/core/terminal.h"
#include "tui/widgets/command_panel.h"
#include "core/events/event_bus.h"
#include "core/config/config_manager.h"
#include "core/task/task_manager.h"

using namespace tui;

namespace {

Terminal null_terminal(&agent::EventBus::instance(),
                       &agent::ConfigManager::instance(),
                       &agent::TaskManager::instance(),
                       TerminalConfig{});

std::vector<CommandEntry> make_commands(size_t count) {
    std::vector<CommandEntry> entries;
    for (size_t i = 0; i < count; ++i) {
        entries.push_back({"cmd" + std::to_string(i), "desc " + std::to_string(i)});
    }
    return entries;
}

} // namespace

TEST_CASE("CommandPanel empty state", "[command_panel][init]") {
    CommandPanel panel(&null_terminal);

    REQUIRE_FALSE(panel.is_active());
    REQUIRE(panel.get_selected() == nullptr);
    REQUIRE(panel.get_completion().empty());
    panel.render();  // 空状态渲染安全
}

TEST_CASE("CommandPanel filter narrows selection", "[command_panel][filter]") {
    CommandPanel panel(&null_terminal);
    panel.set_commands(make_commands(5));
    REQUIRE(panel.is_active());

    panel.set_filter("/cmd1");
    REQUIRE(panel.is_active());
    REQUIRE(panel.get_selected()->name == "cmd1");

    panel.set_filter("/nomatch");
    REQUIRE_FALSE(panel.is_active());
}

TEST_CASE("CommandPanel move wraps around", "[command_panel][navigation]") {
    CommandPanel panel(&null_terminal);
    panel.set_commands(make_commands(3));

    panel.move_up();  // 回绕到最后一个
    REQUIRE(panel.get_selected()->name == "cmd2");

    panel.move_down();
    REQUIRE(panel.get_selected()->name == "cmd0");

    panel.move_down();
    panel.move_down();
    REQUIRE(panel.get_selected()->name == "cmd2");
}

TEST_CASE("CommandPanel large list scrolls within bounds", "[command_panel][scroll]") {
    // 50 个命令（超过 MAX_DISPLAY=10），滚动窗口索引不应越界
    CommandPanel panel(&null_terminal);
    panel.set_commands(make_commands(50));

    for (int i = 0; i < 100; ++i) {
        panel.move_down();
        REQUIRE(panel.get_selected() != nullptr);
    }
    REQUIRE(panel.get_selected()->name == "cmd0");

    for (int i = 0; i < 100; ++i) {
        panel.move_up();
        REQUIRE(panel.get_selected() != nullptr);
    }
    REQUIRE(panel.get_selected()->name == "cmd0");
}

TEST_CASE("CommandPanel get_completion returns slash-prefixed name", "[command_panel][completion]") {
    CommandPanel panel(&null_terminal);
    panel.set_commands(make_commands(2));

    REQUIRE(panel.get_completion() == "/cmd0 ");
    panel.move_down();
    REQUIRE(panel.get_completion() == "/cmd1 ");
}

TEST_CASE("CommandPanel set_visible toggles render state", "[command_panel][visibility]") {
    CommandPanel panel(&null_terminal);
    panel.set_commands(make_commands(3));
    panel.set_visible(true);
    panel.set_visible(false);
    panel.set_visible(true);
    REQUIRE(panel.is_active());
}
