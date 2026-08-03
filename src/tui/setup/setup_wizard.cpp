/**
 * @file setup_wizard.cpp
 * @brief 首次运行设置向导实现
 */

#include "tui/setup/setup_wizard.h"
#include "tui/core/terminal.h"
#include "tui/core/screen.h"
#include "tui/core/platform/i_platform.h"
#include "core/config/i_config_manager.h"

namespace tui {

// ============================================================
// 按键码
// ============================================================

static constexpr char32_t KEY_CTRL_C = 0xE009;
static constexpr char32_t KEY_ESC    = 0x1B;

// ============================================================
// SetupWizard
// ============================================================

SetupWizard::SetupWizard(IPlatform* platform, Terminal* terminal, Screen* screen,
                         agent::IConfigManager& cfg,
                         const std::filesystem::path& config_save_path,
                         ProviderPanelFn provider_panel)
    : m_platform(platform)
    , m_terminal(terminal)
    , m_screen(screen)
    , m_cfg(cfg)
    , m_save_path(config_save_path)
    , m_provider_panel(std::move(provider_panel))
{
}

bool SetupWizard::run_wizard() {
    // 使用实际终端尺寸替代硬编码 80x30
    int term_w = m_terminal->get_terminal_width();
    int term_h = m_terminal->get_terminal_height();
    if (term_w < 40) term_w = 80;  // 兜底：终端宽度过小时回退到默认
    if (term_h < 15) term_h = 30;
    m_screen->resize(term_w, term_h);

    // 欢迎文字
    m_screen->write(0, 2, "欢迎使用 Workx！请设置 API 提供商。", ColorRole::System);
    m_cursor_row = 2;

    // 步骤1：多供应商配置面板（列表 + 表单，含名称/URL/模型/API Key）
    // 面板由 app 层注入（ProviderPanelFn），tui 层不依赖 app/ui/provider_form
    agent::ProviderSwitchResult sel = m_provider_panel(m_cfg, m_terminal, m_screen);
    if (!sel.applied || sel.entry.id.empty()) {
        m_screen->write(m_cursor_row, 2, "设置已取消。使用 --help 查看 CLI 选项。", ColorRole::System);
        m_screen->flush();
        m_platform->flush();
        return false;
    }

    // 步骤2：保存配置（面板已写入 backend.* 标量键与 backend.providers 列表）
    auto result = m_cfg.save_to_file(m_save_path);

    int r = m_cursor_row;
    if (result.is_ok()) {
        m_screen->write(r, 2, "配置已保存！", ColorRole::StatusBar);
        m_screen->write(r + 1, 4, "\xe2\x9c\x93  保存成功！", ColorRole::System);
        m_screen->write(r + 2, 4, std::format("提供商：{}", sel.entry.name), ColorRole::Default);
        if (!sel.entry.model.empty())
            m_screen->write(r + 3, 4, std::format("模型：{}", sel.entry.model), ColorRole::Default);
        m_screen->write(r + 4, 4, std::format("配置：{}", m_save_path.string()), ColorRole::Dim);
        m_screen->write(r + 5, 4, "随时输入 /provider 修改配置。", ColorRole::Dim);
    } else {
        m_screen->write(r, 2, "保存失败", ColorRole::StatusBar);
        m_screen->write(r + 1, 4, "x  " + result.error().to_string(), ColorRole::Error);
        m_screen->write(r + 2, 4, "设置仅本次会话有效。", ColorRole::Dim);
    }

    m_screen->write(r + 6, 4, "按任意键继续...", ColorRole::Dim);
    m_screen->flush();
    m_platform->flush();

    read_key();

    // 完成：清空物理终端 + 重置缓冲区，准备进入主循环
    m_screen->clear_terminal();
    return true;
}

char32_t SetupWizard::read_key() {
    return m_platform->read_char();
}

} // namespace tui
