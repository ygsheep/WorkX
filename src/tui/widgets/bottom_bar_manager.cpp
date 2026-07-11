/**
 * @file bottom_bar_manager.cpp
 * @brief 底部区域管理器实现
 * @version 1.0.0
 */

#include "tui/widgets/bottom_bar_manager.h"
#include "tui/widgets/status_bar.h"
#include "tui/widgets/command_panel.h"
#include "tui/widgets/select_panel.h"
#include "tui/core/terminal.h"
#include "tui/core/screen.h"
#include "tui/core/platform/i_platform.h"
#include "core/events/event_bus.h"

namespace workx {

// Win32 特殊键码
static constexpr char32_t KEY_ARROW_UP   = 0xE002;
static constexpr char32_t KEY_ARROW_DOWN = 0xE003;

BottomBarManager::BottomBarManager(Terminal* terminal)
    : m_terminal(terminal)
    , m_status_bar(std::make_unique<StatusBar>(terminal))
    , m_command_panel(std::make_unique<CommandPanel>(terminal))
    , m_select_panel(nullptr)  // 需要 Screen，在 initialize() 中创建
{
}

BottomBarManager::~BottomBarManager() = default;

StatusBar* BottomBarManager::get_status_bar() const {
    return m_external_sb ? m_external_sb : m_status_bar.get();
}

void BottomBarManager::set_status_bar(StatusBar* sb) {
    m_external_sb = sb;
}

void BottomBarManager::initialize(Screen* screen) {
    m_select_panel = std::make_unique<SelectPanel>(m_terminal, screen);
}

void BottomBarManager::set_mode(BottomBarMode mode) {
    if (m_mode == mode) return;

    int h = m_terminal->get_terminal_height();

    // 离开当前模式：清除旧面板的显示内容
    switch (m_mode) {
        case BottomBarMode::STATUS_BAR:
            get_status_bar()->clear();
            break;
        case BottomBarMode::COMMAND_PANEL:
            m_command_panel->set_visible(false);
            m_command_panel->clear();
            m_terminal->end_overlay();
            break;
        case BottomBarMode::SELECT_PANEL:
            break;
    }

    m_mode = mode;

    // 进入新模式：渲染新面板
    switch (mode) {
        case BottomBarMode::STATUS_BAR:
            get_status_bar()->render();
            break;
        case BottomBarMode::COMMAND_PANEL:
            m_terminal->begin_overlay(h - 8, h - 3);
            m_command_panel->set_visible(true);
            m_command_panel->render();
            break;
        case BottomBarMode::SELECT_PANEL:
            // SelectPanel 自己管理渲染
            break;
    }
}

void BottomBarManager::render() {
    switch (m_mode) {
        case BottomBarMode::STATUS_BAR:
            get_status_bar()->render();
            break;
        case BottomBarMode::COMMAND_PANEL:
            m_command_panel->render();
            break;
        case BottomBarMode::SELECT_PANEL:
            m_select_panel->render();
            break;
    }
}

void BottomBarManager::on_input_changed(const std::string& line) {
    if (line.empty() || line[0] != '/') {
        // 非命令输入 → 回到 StatusBar
        if (m_mode == BottomBarMode::COMMAND_PANEL) {
            set_mode(BottomBarMode::STATUS_BAR);
        }
        return;
    }

    // 输入以 / 开头 → 切换到 CommandPanel 并更新过滤
    if (m_mode != BottomBarMode::COMMAND_PANEL) {
        set_mode(BottomBarMode::COMMAND_PANEL);
    }
    m_command_panel->set_filter(line);
    m_command_panel->render();
}

bool BottomBarManager::handle_navigation(char32_t key) {
    if (m_mode != BottomBarMode::COMMAND_PANEL) return false;
    if (!m_command_panel->is_active()) return false;

    switch (key) {
        case KEY_ARROW_UP:
            m_command_panel->move_up();
            m_command_panel->render();
            return true;
        case KEY_ARROW_DOWN:
            m_command_panel->move_down();
            m_command_panel->render();
            return true;
        default:
            return false;
    }
}

std::string BottomBarManager::handle_tab() {
    if (m_mode != BottomBarMode::COMMAND_PANEL) return "";
    if (!m_command_panel->is_active()) return "";

    return m_command_panel->get_completion();
}

} // namespace workx
