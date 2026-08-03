/**
 * @file setup_wizard.h
 * @brief 首次运行设置向导
 * @details 交互式多供应商配置面板（provider_form）+ 持久化保存
 *          使用 Screen 差分渲染引擎，避免 UI 残留
 * @version 2.0.0
 * @date 2026-08
 */

#pragma once

#include <string>
#include <vector>

namespace agent {
class IConfigManager;
}

namespace tui {

class IPlatform;
class Terminal;
class Screen;

/// @brief 设置向导
/// @details 当首次启动且未配置 Provider 时运行
class SetupWizard {
public:
    /// @brief 构造
    /// @param platform 平台抽象
    /// @param terminal 终端
    /// @param screen 屏幕渲染引擎
    /// @param cfg 配置管理器（多供应商面板读写 backend.* 键 + 持久化）
    SetupWizard(IPlatform* platform, Terminal* terminal, Screen* screen,
                agent::IConfigManager& cfg);

    /// @brief 运行设置向导
    /// @return true 配置成功，false 用户取消
    bool run_wizard();

private:
    char32_t read_key();

    IPlatform* m_platform;
    Terminal* m_terminal;
    Screen* m_screen;               ///< 差分渲染引擎
    agent::IConfigManager& m_cfg;   ///< 配置管理器（读 + 写 + 持久化）

    int m_cursor_row = 0;           ///< Screen 中当前写入行号
};

} // namespace tui
