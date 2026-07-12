/**
 * @file setup_wizard.h
 * @brief 首次运行设置向导
 * @details 交互式选择 API Provider + 输入 API Key + 持久化保存
 *          使用 Screen 差分渲染引擎，避免 UI 残留
 * @version 1.2.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <vector>
#include "core/config/config_manager.h"
#include "agent/model/provider_preset.h"
#include "tui/core/screen.h"

namespace agent {

class IPlatform;
class Terminal;

/// @brief 设置向导
/// @details 当首次启动且未配置 Provider 时运行
class SetupWizard {
public:
    SetupWizard(IPlatform* platform, Terminal* terminal, Screen* screen);

    /// @brief 运行设置向导
    /// @return true 配置成功，false 用户取消
    bool run_wizard();

private:
    /// @brief 步骤1：选择 API Provider
    const ProviderPreset* select_provider();

    /// @brief 自定义流程：选择协议类型（OpenAI / Anthropic）
    ProviderType select_protocol();

    /// @brief 输入 API Key（* 掩码，可选是否必填）
    std::string prompt_api_key(const std::string& provider_name, bool required);

    /// @brief 输入 URL
    std::string prompt_url();

    /// @brief 输入模型名
    std::string prompt_model();

    /// @brief 保存并显示确认
    void save_and_confirm(const std::string& provider_name,
                          const std::string& display_name,
                          const std::string& api_key,
                          const std::string& model_name,
                          const std::string& remote_url = "");

    // ---- 渲染 ----
    void draw_provider_list(int selected, const std::vector<const ProviderPreset*>& presets);

    /// @brief 简单的协议选项列表
    void draw_option_list(const std::vector<std::string>& options, int selected);

    /// @brief 在 Screen 上绘制一行提示（行号递增）
    void draw_hint(int row, const std::string& text);

    char32_t read_key();
    std::string read_input_line(const std::string& prompt_text, bool mask = false);

    IPlatform* m_platform;
    Terminal* m_terminal;
    Screen* m_screen;           ///< 差分渲染引擎

    int m_cursor_row = 0;       ///< Screen 中当前写入行号
};

} // namespace workx
