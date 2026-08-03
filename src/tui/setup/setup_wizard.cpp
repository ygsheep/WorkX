/**
 * @file setup_wizard.cpp
 * @brief 首次运行设置向导实现
 */

#include "tui/setup/setup_wizard.h"
#include "tui/core/terminal.h"
#include "tui/core/platform/i_platform.h"
#include "tui/utils/utf8_utils.h"
#include "core/config/i_config_manager.h"
#include "app/config/app_config.h"

namespace tui {

using namespace agent;  // P0: tui→agent 类型引用过渡方案，后续 P2/P3 收紧到显式前缀

// E.10：UTF-8 编码辅助函数（setup_wizard 内部使用）
// 将 codepoint 编码为 UTF-8 并追加到 out
static void append_utf8(char32_t cp, std::string& out) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// E.10：返回 s 中最后一个完整 UTF-8 字符的起始字节位置
// 遇到续字节（0x80-0xBF）向前回溯，直到找到起始字节
static size_t last_utf8_char_start(const std::string& s) {
    if (s.empty()) return 0;
    size_t pos = s.size() - 1;
    while (pos > 0 && (static_cast<unsigned char>(s[pos]) & 0xC0) == 0x80) {
        --pos;
    }
    return pos;
}

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

// 使用 app/config/app_config.h 中的全局 default_config_path()，
// 与 main.cpp 加载路径保持一致（~/.workx/config.json）。
// 注意：不得在此定义同名 static 函数，否则遮蔽全局版，
// 导致向导保存路径与启动加载路径不一致。

// ============================================================
// SetupWizard
// ============================================================

SetupWizard::SetupWizard(IPlatform* platform, Terminal* terminal, Screen* screen,
                         agent::IConfigWriter& writer,
                         agent::IConfigPersistence& persistence)
    : m_platform(platform)
    , m_terminal(terminal)
    , m_screen(screen)
    , m_writer(writer)
    , m_persistence(persistence)
{
}

bool SetupWizard::run_wizard() {
    // E.10：使用实际终端尺寸替代硬编码 80x30
    int term_w = m_terminal->get_terminal_width();
    int term_h = m_terminal->get_terminal_height();
    if (term_w < 40) term_w = 80;  // 兜底：终端宽度过小时回退到默认
    if (term_h < 15) term_h = 30;
    m_screen->resize(term_w, term_h);

    // 欢迎文字
    m_screen->write(0, 2, "欢迎使用 Workx！请设置 API 提供商。", ColorRole::System);
    m_cursor_row = 2;

    // 步骤1：选择 Provider
    const ProviderPreset* preset = select_provider();
    if (!preset) {
        m_screen->write(m_cursor_row, 2, "设置已取消。使用 --help 查看 CLI 选项。", ColorRole::System);
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
        api_key_required = true;  // 中国模型提供商均需 API Key
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
        m_screen->write(list_start, 2, "选择 API 提供商：", ColorRole::StatusBar);

        // 画列表
        for (int i = 0; i < static_cast<int>(presets.size()); i++) {
            int r = list_start + 1 + i;
            const auto* p = presets[i];
            bool custom = (p->name == "openai-compatible");
            std::string url = custom
                ? "(自定义 URL + 协议)"
                : build_preset_url(p).value_or("(无默认 URL)");

            const char* bullet = (i == selected) ? "\xe2\x97\x8f" : "\xe2\x97\x8b";
            std::string text = std::format("{} {:<18} \xe2\x86\x92 {}",
                                           bullet, p->display_name, url);
            ColorRole color = (i == selected) ? ColorRole::Prompt : ColorRole::Default;
            m_screen->write(r, 4, text, color);
        }

        // 底部提示
        int hint_row = list_start + 1 + static_cast<int>(presets.size());
        m_screen->write(hint_row, 4, "\xe2\x86\x91\xe2\x86\x93 导航  回车选择  q 退出",
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
        m_screen->write(start_row, 2, "选择协议类型：", ColorRole::StatusBar);
        for (int i = 0; i < static_cast<int>(options.size()); i++) {
            const char* bullet = (i == selected) ? "\xe2\x97\x8f" : "\xe2\x97\x8b";
            ColorRole color = (i == selected) ? ColorRole::Prompt : ColorRole::Default;
            m_screen->write(start_row + 1 + i, 4,
                           std::format("{} {}", bullet, options[i]), color);
        }
        m_screen->write(start_row + 1 + static_cast<int>(options.size()), 4,
                        "\xe2\x86\x91\xe2\x86\x93 导航  回车选择", ColorRole::Dim);
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
    m_screen->write(m_cursor_row, 2, "输入自定义 API 地址：", ColorRole::StatusBar);
    m_screen->write(m_cursor_row + 1, 4, "示例：http://localhost:1234", ColorRole::Dim);
    m_screen->flush();
    m_terminal->write(std::format("\x1b[{};1H", m_cursor_row + 3));
    return read_input_line("Base URL", false);
}

std::string SetupWizard::prompt_model() {
    m_cursor_row += 2;
    m_screen->write(m_cursor_row, 2, "输入模型名称：", ColorRole::StatusBar);
    m_screen->write(m_cursor_row + 1, 4, "留空可稍后设置。", ColorRole::Dim);
    m_screen->flush();
    m_terminal->write(std::format("\x1b[{};1H", m_cursor_row + 2));
    return read_input_line("Model Name", false);
}

std::string SetupWizard::prompt_api_key(const std::string& provider_name, bool required) {
    m_screen->write(m_cursor_row, 2,
                    std::format("输入 {} 的 API Key：", provider_name), ColorRole::StatusBar);
    int input_row = m_cursor_row + 1;
    if (!required) {
        m_screen->write(m_cursor_row + 1, 4,
                        "本地服务（如 LM Studio）可留空。", ColorRole::Dim);
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
    // E.10：跟踪已输入字符的总显示宽度，用于 CTRL_U 退格
    int input_display_width = 0;

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
                // E.10：找到最后一个完整 UTF-8 字符的起始字节
                size_t last_start = last_utf8_char_start(input);
                std::string last_char = input.substr(last_start);
                input.erase(last_start);  // 移除整个 UTF-8 字符（可能 1-4 字节）
                // 用 char32_width 计算显示宽度，退格对应数量的单元格
                // 先解码 last_char 为 codepoint
                char32_t cp = 0;
                if (last_char.size() == 1) {
                    cp = static_cast<unsigned char>(last_char[0]);
                } else if (last_char.size() == 2) {
                    cp = ((static_cast<unsigned char>(last_char[0]) & 0x1F) << 6)
                       | (static_cast<unsigned char>(last_char[1]) & 0x3F);
                } else if (last_char.size() == 3) {
                    cp = ((static_cast<unsigned char>(last_char[0]) & 0x0F) << 12)
                       | ((static_cast<unsigned char>(last_char[1]) & 0x3F) << 6)
                       | (static_cast<unsigned char>(last_char[2]) & 0x3F);
                } else if (last_char.size() == 4) {
                    cp = ((static_cast<unsigned char>(last_char[0]) & 0x07) << 18)
                       | ((static_cast<unsigned char>(last_char[1]) & 0x3F) << 12)
                       | ((static_cast<unsigned char>(last_char[2]) & 0x3F) << 6)
                       | (static_cast<unsigned char>(last_char[3]) & 0x3F);
                }
                int w = char32_width(cp);
                if (w < 1) w = 1;
                input_display_width -= w;
                for (int i = 0; i < w; ++i) {
                    m_terminal->write("\b \b");
                }
            }
        } else if (ch == KEY_CTRL_U) {
            // E.10：用显示宽度退格，而非字节数
            for (int i = 0; i < input_display_width; ++i) {
                m_terminal->write("\b \b");
            }
            input.clear();
            input_display_width = 0;
        } else if (ch >= 0x20 && ch != 0x7F) {
            // E.10：接受任意 Unicode 可打印字符（移除上限 0x7F），支持中文 API Key
            std::string utf8_char;
            append_utf8(ch, utf8_char);
            input += utf8_char;
            int w = char32_width(ch);
            if (w < 1) w = 1;
            input_display_width += w;
            if (mask) {
                // 密码模式：每个字符显示为 *，按显示宽度补齐
                for (int i = 0; i < w; ++i) {
                    m_terminal->write("*");
                }
            } else {
                m_terminal->write(utf8_char);
            }
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
    // H-A：使用 IConfigWriter 写配置，IConfigPersistence 持久化
    m_writer.set("backend.provider", provider_name);
    m_writer.set("backend.api_key", api_key);
    if (!model_name.empty()) m_writer.set("backend.model_name", model_name);
    if (!remote_url.empty()) m_writer.set("backend.remote_url", remote_url);

    auto save_path = default_config_path();
    auto result = m_persistence.save_to_file(save_path);

    m_cursor_row++;
    int r = m_cursor_row;

    if (result.is_ok()) {
        m_screen->write(r, 2, "配置已保存！", ColorRole::StatusBar);
        m_screen->write(r + 1, 4, "\xe2\x9c\x93  保存成功！", ColorRole::System);
        m_screen->write(r + 2, 4, std::format("提供商：{}", display_name), ColorRole::Default);
        if (!model_name.empty())
            m_screen->write(r + 3, 4, std::format("模型：{}", model_name), ColorRole::Default);
        m_screen->write(r + 4, 4, std::format("配置：{}", save_path.string()), ColorRole::Dim);
        m_screen->write(r + 5, 4, "使用 --provider 或编辑配置来更改。", ColorRole::Dim);
    } else {
        m_screen->write(r, 2, "保存失败", ColorRole::StatusBar);
        m_screen->write(r + 1, 4, "x  " + result.error().to_string(), ColorRole::Error);
        m_screen->write(r + 2, 4, "设置仅本次会话有效。", ColorRole::Dim);
    }

    m_screen->write(r + 6, 4, "按任意键继续...", ColorRole::Dim);
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

} // namespace tui
