/**
 * @file setup_wizard.cpp
 * @brief 首次运行设置向导实现
 */

#include "tui/setup/setup_wizard.h"
#include "tui/core/terminal.h"
#include "tui/core/platform/i_platform.h"

#include <filesystem>

namespace agent {

// ============================================================
// 按键码
// ============================================================

static constexpr char32_t KEY_ARROW_UP    = 0xE002;
static constexpr char32_t KEY_ARROW_DOWN  = 0xE003;
static constexpr char32_t KEY_CTRL_C      = 0xE009;
static constexpr char32_t KEY_ENTER       = 0x0D;
static constexpr char32_t KEY_ESC         = 0x1B;
static constexpr char32_t KEY_BACKSPACE   = 0x7F;
static constexpr char32_t KEY_CTRL_U      = 0x15;

// ============================================================
// 默认配置文件路径
// ============================================================

static std::filesystem::path default_config_path() {
#ifdef _WIN32
    const char* appdata = std::getenv("APPDATA");
    if (appdata) {
        return std::filesystem::path(appdata) / "workx" / "config.json";
    }
    return std::filesystem::path("workx.json");
#else
    const char* home = std::getenv("HOME");
    if (home) {
        return std::filesystem::path(home) / ".config" / "workx" / "config.json";
    }
    return std::filesystem::path("workx.json");
#endif
}

// ============================================================
// SetupWizard
// ============================================================

SetupWizard::SetupWizard(IPlatform* platform, Terminal* terminal, Screen* screen)
    : m_platform(platform)
    , m_terminal(terminal)
    , m_screen(screen)
{
}

bool SetupWizard::run_wizard() {
    m_screen->resize(80, 30);

    // 欢迎文字
    m_screen->write(0, 2, "Welcome to Workx! Let's set up your API provider.", ColorRole::System);
    m_cursor_row = 2;

    // 步骤1：选择 Provider
    const ProviderPreset* preset = select_provider();
    if (!preset) {
        m_screen->write(m_cursor_row, 2, "Setup cancelled. Use --help to see CLI options.", ColorRole::System);
        m_screen->flush();
        m_platform->flush();
        return false;
    }

    bool is_custom = (std::string(preset->name) == "openai-compatible");
    std::string save_provider_name;
    std::string display_name;
    std::string remote_url;
    std::string model_name;
    bool api_key_required = true;

    if (is_custom) {
        // ---- 自定义流程 ----
        m_cursor_row++;
        ProviderType protocol = select_protocol();
        display_name = (protocol == ProviderType::OpenAI) ? "OpenAI (Custom)" : "Anthropic (Custom)";
        save_provider_name = (protocol == ProviderType::OpenAI) ? "openai" : "anthropic";

        m_cursor_row++;
        remote_url = prompt_url();
        if (remote_url.empty()) return false;

        model_name = prompt_model();
        api_key_required = false;
    } else {
        // ---- 命名 Preset ----
        save_provider_name = std::string(preset->name);
        display_name = std::string(preset->display_name);
        model_name = std::string(preset->default_model);
        api_key_required = (std::string(preset->name) != "lm-studio");
    }

    // 步骤2：输入 API Key
    m_cursor_row++;
    std::string api_key = prompt_api_key(display_name, api_key_required);
    if (api_key.empty() && api_key_required) return false;

    // 步骤3：保存并确认
    save_and_confirm(save_provider_name, display_name, api_key, model_name, remote_url);

    // 完成：清空物理终端 + 重置缓冲区，准备进入主循环
    m_screen->clear_terminal();
    return true;
}

// ============================================================
// Provider 选择
// ============================================================

const ProviderPreset* SetupWizard::select_provider() {
    auto all_names = list_preset_names();
    std::vector<const ProviderPreset*> presets;
    const ProviderPreset* custom_preset = nullptr;

    for (auto name : all_names) {
        auto* p = find_preset(name);
        if (!p) continue;
        if (std::string(p->name) == "openai-compatible") {
            custom_preset = p;
        } else {
            presets.push_back(p);
        }
    }
    if (custom_preset) presets.push_back(custom_preset);

    if (presets.empty()) return nullptr;

    int selected = 0;
    const int list_start = m_cursor_row;

    while (true) {
        // 画标题
        m_screen->write(list_start, 2, "Select API Provider:", ColorRole::StatusBar);

        // 画列表
        for (int i = 0; i < static_cast<int>(presets.size()); i++) {
            int r = list_start + 1 + i;
            const auto* p = presets[i];
            bool custom = (std::string(p->name) == "openai-compatible");
            std::string url = custom ? "(custom URL + protocol)" : build_preset_url(p);

            const char* bullet = (i == selected) ? "\xe2\x97\x8f" : "\xe2\x97\x8b";
            std::string text = std::format("{} {:<18} \xe2\x86\x92 {}",
                                           bullet, p->display_name, url);
            ColorRole color = (i == selected) ? ColorRole::Prompt : ColorRole::Default;
            m_screen->write(r, 4, text, color);
        }

        // 底部提示
        int hint_row = list_start + 1 + static_cast<int>(presets.size());
        m_screen->write(hint_row, 4, "\xe2\x86\x91\xe2\x86\x93 navigate  Enter select  q quit",
                        ColorRole::Dim);

        m_screen->flush();
        m_platform->flush();

        char32_t key = read_key();

        switch (key) {
            case KEY_ARROW_UP:
                if (selected > 0) selected--;
                break;
            case KEY_ARROW_DOWN:
                if (selected < static_cast<int>(presets.size()) - 1) selected++;
                break;
            case KEY_ENTER:
                m_cursor_row = hint_row + 1;
                return presets[selected];
            case 'q': case 'Q': case KEY_ESC: case KEY_CTRL_C:
                return nullptr;
            default:
                break;
        }
    }
}

// ============================================================
// 协议选择
// ============================================================

ProviderType SetupWizard::select_protocol() {
    std::vector<std::string> options = {"OpenAI", "Anthropic"};
    int selected = 0;
    const int start_row = m_cursor_row;

    while (true) {
        m_screen->write(start_row, 2, "Select Protocol Type:", ColorRole::StatusBar);
        for (int i = 0; i < static_cast<int>(options.size()); i++) {
            const char* bullet = (i == selected) ? "\xe2\x97\x8f" : "\xe2\x97\x8b";
            ColorRole color = (i == selected) ? ColorRole::Prompt : ColorRole::Default;
            m_screen->write(start_row + 1 + i, 4,
                           std::format("{} {}", bullet, options[i]), color);
        }
        m_screen->write(start_row + 1 + static_cast<int>(options.size()), 4,
                        "\xe2\x86\x91\xe2\x86\x93 navigate  Enter select", ColorRole::Dim);
        m_screen->flush();
        m_platform->flush();

        char32_t key = read_key();
        switch (key) {
            case KEY_ARROW_UP:    if (selected > 0) selected--; break;
            case KEY_ARROW_DOWN:  if (selected < 1) selected++; break;
            case KEY_ENTER:
                m_cursor_row = start_row + 3;
                return (selected == 0) ? ProviderType::OpenAI : ProviderType::Anthropic;
            case KEY_ESC: case KEY_CTRL_C: case 'q':
                return ProviderType::OpenAI;
            default: break;
        }
    }
}

// ============================================================
// 输入辅助
// ============================================================

std::string SetupWizard::prompt_url() {
    m_screen->write(m_cursor_row, 2, "Enter Custom API Base URL:", ColorRole::StatusBar);
    m_screen->write(m_cursor_row + 1, 4, "Example: http://localhost:1234", ColorRole::Dim);
    m_screen->flush();
    m_terminal->write(std::format("\x1b[{};1H", m_cursor_row + 3));
    return read_input_line("Base URL", false);
}

std::string SetupWizard::prompt_model() {
    m_cursor_row += 2;
    m_screen->write(m_cursor_row, 2, "Enter Model Name:", ColorRole::StatusBar);
    m_screen->write(m_cursor_row + 1, 4, "Leave empty to set later.", ColorRole::Dim);
    m_screen->flush();
    m_terminal->write(std::format("\x1b[{};1H", m_cursor_row + 2));
    return read_input_line("Model Name", false);
}

std::string SetupWizard::prompt_api_key(const std::string& provider_name, bool required) {
    m_screen->write(m_cursor_row, 2,
                    std::format("Enter API Key for {}:", provider_name), ColorRole::StatusBar);
    int input_row = m_cursor_row + 1;
    if (!required) {
        m_screen->write(m_cursor_row + 1, 4,
                        "Leave empty for local servers (e.g. LM Studio).", ColorRole::Dim);
        input_row++;
    }
    m_screen->flush();

    // 定位光标到输入行起始位置
    m_terminal->write(std::format("\x1b[{};1H", input_row + 1));

    std::string key = read_input_line("API Key", true);
    if (key.empty() && !required) {
        m_cursor_row += 2;
        return key;
    }
    if (key.empty()) return "";
    m_cursor_row += 2;
    return key;
}

std::string SetupWizard::read_input_line(const std::string& prompt_text, bool mask) {
    std::string input;

    // 直接在终端输出提示（不走 Screen，需要实时回显）
    m_terminal->set_color(ColorRole::Prompt);
    m_terminal->write(std::format("  {}: ", prompt_text));
    m_terminal->reset_color();

    while (true) {
        m_platform->flush();
        char32_t ch = read_key();

        if (ch == KEY_ENTER) {
            if (input.empty()) continue;
            m_terminal->write("\n");
            return input;
        } else if (ch == KEY_ESC || ch == KEY_CTRL_C || ch == 'q') {
            m_terminal->write("\n");
            return "";
        } else if (ch == KEY_BACKSPACE || ch == '\b') {
            if (!input.empty()) {
                input.pop_back();
                m_terminal->write("\b \b");
            }
        } else if (ch == KEY_CTRL_U) {
            for (size_t i = 0; i < input.size(); i++)
                m_terminal->write("\b \b");
            input.clear();
        } else if (ch >= 0x20 && ch < 0x7F) {
            input += static_cast<char>(ch);
            m_terminal->write(mask ? "*" : std::string(1, static_cast<char>(ch)));
        }
    }
}

// ============================================================
// 保存确认
// ============================================================

void SetupWizard::save_and_confirm(const std::string& provider_name,
                                    const std::string& display_name,
                                    const std::string& api_key,
                                    const std::string& model_name,
                                    const std::string& remote_url) {
    auto& cfg = ConfigManager::instance();

    cfg.set("backend.provider", provider_name);
    cfg.set("backend.api_key", api_key);
    if (!model_name.empty()) cfg.set("backend.model_name", model_name);
    if (!remote_url.empty()) cfg.set("backend.remote_url", remote_url);

    auto save_path = default_config_path();
    auto result = cfg.save_to_file(save_path);

    m_cursor_row++;
    int r = m_cursor_row;

    if (result.isOk()) {
        m_screen->write(r, 2, "Configuration Saved!", ColorRole::StatusBar);
        m_screen->write(r + 1, 4, "\xe2\x9c\x93  Saved successfully!", ColorRole::System);
        m_screen->write(r + 2, 4, std::format("Provider: {}", display_name), ColorRole::Default);
        if (!model_name.empty())
            m_screen->write(r + 3, 4, std::format("Model: {}", model_name), ColorRole::Default);
        m_screen->write(r + 4, 4, std::format("Config: {}", save_path.string()), ColorRole::Dim);
        m_screen->write(r + 5, 4, "Use --provider or edit config to change.", ColorRole::Dim);
    } else {
        m_screen->write(r, 2, "Save failed", ColorRole::StatusBar);
        m_screen->write(r + 1, 4, "x  " + result.error(), ColorRole::Error);
        m_screen->write(r + 2, 4, "Settings used for this session only.", ColorRole::Dim);
    }

    m_screen->write(r + 6, 4, "Press any key to continue...", ColorRole::Dim);
    m_screen->flush();
    m_platform->flush();

    read_key();
}

// ============================================================
// 工具函数
// ============================================================

char32_t SetupWizard::read_key() {
    return m_platform->read_char();
}

} // namespace workx
