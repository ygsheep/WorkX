/**
 * @file provider_selector.cpp
 * @brief 交互式供应商选择实现
 * @details 使用 SelectPanel 展示供应商预设列表，支持上下选择与内联输入。
 *          custom 预设额外采集 URL / 模型名 / API Key。
 * @version 1.0.0
 * @date 2026-08
 */

#include <algorithm>
#include <string>
#include <vector>

#include "agent/model/provider_preset.h"
#include "app/config/app_config.h"
#include "app/ui/provider_selector.h"
#include "core/config/i_config_manager.h"
#include "tui/core/platform/i_platform.h"
#include "tui/core/screen.h"
#include "tui/core/terminal.h"
#include "tui/widgets/select_panel.h"

namespace agent {

using namespace tui;  // 对齐 model_selector.cpp 的过渡方案

namespace {

constexpr char32_t KEY_UP    = 0xE002;
constexpr char32_t KEY_DOWN  = 0xE003;
constexpr char32_t KEY_TAB   = 0x09;
constexpr char32_t KEY_ENTER = 0x0D;
constexpr char32_t KEY_ESC   = 0x1B;
constexpr char32_t KEY_SPACE = 0x20;
constexpr char32_t KEY_CTRL_C = 0xE009;

/// "Custom URL..." 选项的 sentinel id
constexpr const char* CUSTOM_PROVIDER_ID = "openai-compatible";

/// @brief 内联输入阶段
enum class InputStage {
    None = 0,   ///< 选择模式
    ApiKey,     ///< 输入 API Key
    CustomUrl,  ///< custom：输入 URL
    CustomModel ///< custom：输入模型名
};

} // anonymous namespace

ProviderSelection select_provider_interactive(
    IConfigManager& cfg,
    Terminal* term, Screen* scr)
{
    ProviderSelection result;

    // 构建预设列表（custom 置底）
    auto all_names = list_preset_names();
    std::vector<const ProviderPreset*> presets;
    const ProviderPreset* custom = nullptr;
    for (auto name : all_names) {
        auto* p = find_preset(name);
        if (!p) continue;
        if (p->name == CUSTOM_PROVIDER_ID) custom = p;
        else presets.push_back(p);
    }
    if (custom) presets.push_back(custom);

    if (presets.empty()) {
        term->set_color(ColorRole::Error);
        term->write("No providers available.\n");
        term->reset_color();
        return result;
    }

    int scroll_bottom = term->get_terminal_height() - 3;
    if (scroll_bottom < 1) scroll_bottom = 1;
    term->begin_overlay(1, scroll_bottom);

    // 构建 SelectPanel 数据
    SelectPanel select_panel(term, scr);
    std::vector<SelectItem> items;
    items.reserve(presets.size());
    for (const auto* p : presets) {
        bool is_custom = (p->name == CUSTOM_PROVIDER_ID);
        std::string display = is_custom
            ? std::string("Custom URL (\xe8\x87\xaa\xe5\xae\x9a\xe4\xb9\x89\xe5\x9c\xb0\xe5\x9d\x80)")
            : p->display_name;
        items.push_back({std::string(p->name), std::move(display), false});
    }
    select_panel.set_tabs({{"Providers", items}});
    select_panel.set_title("Select Provider");

    // 初始光标定位到当前 provider
    std::string cur_provider = cfg.get_or<std::string>(keys::PROVIDER, "");
    auto it = std::find_if(items.begin(), items.end(),
        [&](const SelectItem& item) { return item.id == cur_provider; });
    if (it != items.end()) {
        int i = static_cast<int>(std::distance(items.begin(), it));
        for (int j = 0; j < i; j++) select_panel.move_down();
    }

    // 交互循环
    InputStage stage = InputStage::None;
    const ProviderPreset* chosen_preset = nullptr;
    std::string api_key;
    std::string custom_url;
    std::string custom_model;

    while (true) {
        select_panel.render();
        char32_t key = term->platform()->read_char();

        if (select_panel.is_input_mode()) {
            // ---- 内联输入模式 ----
            switch (key) {
                case KEY_ENTER: {
                    std::string text = select_panel.get_input_text();
                    switch (stage) {
                        case InputStage::ApiKey:
                            api_key = std::move(text);
                            if (chosen_preset && chosen_preset->name == CUSTOM_PROVIDER_ID) {
                                stage = InputStage::CustomUrl;
                                select_panel.set_title("Base URL (e.g. http://localhost:1234)");
                                select_panel.activate_input_mode();
                            } else {
                                select_panel.dismiss();
                                term->end_overlay();
                                scr->reset_buffers();
                                result.provider = chosen_preset->name;
                                result.remote_url = build_preset_url(chosen_preset).value_or("");
                                result.api_path = chosen_preset->api_path;
                                result.model_name = chosen_preset->default_model;
                                result.api_key = std::move(api_key);
                                return result;
                            }
                            break;
                        case InputStage::CustomUrl:
                            custom_url = std::move(text);
                            stage = InputStage::CustomModel;
                            select_panel.set_title("Model Name (empty = skip)");
                            select_panel.activate_input_mode();
                            break;
                        case InputStage::CustomModel:
                            custom_model = std::move(text);
                            select_panel.dismiss();
                            term->end_overlay();
                            scr->reset_buffers();
                            result.provider = CUSTOM_PROVIDER_ID;
                            result.remote_url = std::move(custom_url);
                            result.api_path = chosen_preset ? chosen_preset->api_path : "/v1/chat/completions";
                            result.model_name = std::move(custom_model);
                            result.api_key = std::move(api_key);
                            return result;
                        default:
                            break;
                    }
                    break;
                }
                case KEY_ESC:
                case KEY_CTRL_C:
                    select_panel.dismiss();
                    term->end_overlay();
                    scr->reset_buffers();
                    return {};
                case 0x08: case 0x7F:  // Backspace
                    select_panel.input_backspace();
                    break;
                default:
                    if (key >= 0x20 && key <= 0x7E) {
                        select_panel.input_char(key);
                    }
                    break;
            }
        } else {
            // ---- 选择模式 ----
            switch (key) {
                case KEY_UP:    select_panel.move_up(); break;
                case KEY_DOWN:  select_panel.move_down(); break;
                case KEY_TAB:   select_panel.move_down(); break;
                case KEY_SPACE:
                case KEY_ENTER: {
                    std::string chosen;
                    auto selected_ids = select_panel.get_selected_ids();
                    if (!selected_ids.empty()) {
                        chosen = selected_ids[0];
                    } else if (auto* cur = select_panel.get_current_item()) {
                        chosen = cur->id;
                    }
                    if (chosen.empty()) break;
                    chosen_preset = find_preset(chosen);
                    if (!chosen_preset) break;
                    stage = InputStage::ApiKey;
                    select_panel.set_title("API Key (paste, then Enter)");
                    select_panel.activate_input_mode();
                    break;
                }
                case 'q': case 'Q': case KEY_ESC: case KEY_CTRL_C:
                    select_panel.dismiss();
                    term->end_overlay();
                    scr->reset_buffers();
                    return {};
                default: break;
            }
        }
    }
}

void apply_provider_selection(IConfigManager& cfg, const ProviderSelection& sel) {
    if (sel.provider.empty()) return;
    cfg.set(keys::PROVIDER, sel.provider);
    if (!sel.remote_url.empty()) {
        cfg.set(keys::REMOTE_URL, sel.remote_url);
    }
    if (!sel.api_key.empty()) {
        cfg.set(keys::API_KEY, sel.api_key);
    }
    if (!sel.model_name.empty()) {
        cfg.set(keys::MODEL_NAME, sel.model_name);
    }
    // 清除 context_length：resolve_context_length 用 >0 判断是否使用用户配置，
    // 置 0 等价于"未设置"，让 catalog/静态表重新解析新供应商模型窗口。
    if (cfg.has(keys::CONTEXT_LENGTH)) {
        cfg.set(keys::CONTEXT_LENGTH, 0);
    }
}

} // namespace agent
