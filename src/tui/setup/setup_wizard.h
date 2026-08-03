/**
 * @file setup_wizard.h
 * @brief 首次运行设置向导
 * @details 交互式多供应商配置面板 + 持久化保存
 *          使用 Screen 差分渲染引擎，避免 UI 残留
 *          面板通过回调注入（由 app 层提供），保持 tui 层不依赖 app 层
 * @version 2.0.0
 * @date 2026-08
 */

#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "agent/model/provider_config.h"

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
    /// 多供应商管理面板回调（app 层注入，避免 tui 依赖 app/ui/provider_form）
    using ProviderPanelFn = std::function<agent::ProviderSwitchResult(
        agent::IConfigManager&, tui::Terminal*, tui::Screen*)>;

    /// @brief 构造
    /// @param platform 平台抽象
    /// @param terminal 终端
    /// @param screen 屏幕渲染引擎
    /// @param cfg 配置管理器（多供应商面板读写 backend.* 键 + 持久化）
    /// @param config_save_path 配置保存路径（向导完成时 save_to_file）
    /// @param provider_panel 供应商管理面板回调
    SetupWizard(IPlatform* platform, Terminal* terminal, Screen* screen,
                agent::IConfigManager& cfg,
                const std::filesystem::path& config_save_path,
                ProviderPanelFn provider_panel);

    /// @brief 运行设置向导
    /// @return true 配置成功，false 用户取消
    bool run_wizard();

private:
    char32_t read_key();

    IPlatform* m_platform;
    Terminal* m_terminal;
    Screen* m_screen;               ///< 差分渲染引擎
    agent::IConfigManager& m_cfg;   ///< 配置管理器（读 + 写 + 持久化）
    std::filesystem::path m_save_path;  ///< 配置保存路径
    ProviderPanelFn m_provider_panel;   ///< 供应商管理面板回调（app 层注入）

    int m_cursor_row = 0;           ///< Screen 中当前写入行号
};

} // namespace tui
