/**
 * @file model_selector.cpp
 * @brief 交互式模型选择实现
 * @details 使用 SelectPanel 展示模型列表，支持上下选择和自定义输入
 * @version 1.0.0
 * @date 2026-07
 */

#include <algorithm>
#include <vector>

#include "agent/api/chat_types.h"
#include "agent/api/i_backend.h"
#include "agent/model/provider_preset.h"
#include "app/config/app_config.h"
#include "app/ui/model_selector.h"
#include "core/config/config_manager.h"
#include "tui/core/platform/i_platform.h"
#include "tui/core/screen.h"
#include "tui/core/terminal.h"
#include "tui/widgets/select_panel.h"

namespace agent {

/// "Custom Model..." 选项的 sentinel id
static constexpr const char* CUSTOM_MODEL_ID = "__custom__";

std::string select_model_interactive(
    Terminal* term, Screen* scr, IBackend* bk,
    const std::string& current_model)
{
    constexpr char32_t KEY_UP    = 0xE002;
    constexpr char32_t KEY_DOWN  = 0xE003;
    constexpr char32_t KEY_TAB   = 0x09;
    constexpr char32_t KEY_ENTER = 0x0D;
    constexpr char32_t KEY_ESC   = 0x1B;
    constexpr char32_t KEY_SPACE = 0x20;
    constexpr char32_t KEY_CTRL_C = 0xE009;

    auto& cfg = ConfigManager::instance();

    // 获取 base_url
    std::string base_url;
    if (cfg.has(keys::REMOTE_URL)) {
        base_url = cfg.get_or<std::string>(keys::REMOTE_URL, "");
    } else {
        std::string pname = cfg.get_or<std::string>(keys::PROVIDER, "");
        auto* preset = pname.empty() ? nullptr : find_preset(pname);
        if (preset && !preset->default_url.empty()) {
            base_url = std::string(preset->default_url);
        }
    }

    if (base_url.empty()) {
        term->set_color(ColorRole::Error);
        term->write("No API URL configured. Use --provider or --remote first.\n");
        term->reset_color();
        return "";
    }

    int scroll_bottom = term->get_terminal_height() - 3;
    if (scroll_bottom < 1) scroll_bottom = 1;
    term->begin_overlay(1, scroll_bottom);

    scr->write(0, 2, "Fetching models...", ColorRole::StatusBar);
    scr->flush();

    // 获取模型列表
    std::vector<std::string> model_names;
    auto result = bk ? bk->list_models() : Result<std::vector<ModelInfo>, std::string>::err("No backend");

    if (result.isOk()) {
        auto models = result.unwrap();
        model_names.reserve(models.size());
        std::transform(models.begin(), models.end(), std::back_inserter(model_names),
            [](auto& m) { return std::move(m.name); });
    }

    if (model_names.empty()) {
        if (!current_model.empty()) model_names.push_back(current_model);
        model_names.emplace_back("gpt-4o");
        model_names.emplace_back("gpt-4o-mini");
        model_names.emplace_back("deepseek-chat");
        model_names.emplace_back("claude-sonnet-4-20250514");
    }

    // 构建 SelectPanel 数据
    SelectPanel select_panel(term, scr);
    std::vector<SelectItem> items;
    items.reserve(model_names.size() + 1);
    std::transform(model_names.begin(), model_names.end(), std::back_inserter(items),
        [](const auto& name) { return SelectItem{name, name, false}; });
    items.push_back({CUSTOM_MODEL_ID, "Custom Model...", false});
    select_panel.set_tabs({{"Models", items}});
    select_panel.set_title("Select Model");

    // 设置初始光标到当前模型
    auto it = std::find_if(items.begin(), items.end(),
        [&](const SelectItem& item) { return item.id == current_model; });
    if (it != items.end()) {
        int i = static_cast<int>(std::distance(items.begin(), it));
        for (int j = 0; j < i; j++) select_panel.move_down();
    }

    // 交互循环
    while (true) {
        select_panel.render();
        char32_t key = term->platform()->read_char();

        if (select_panel.is_input_mode()) {
            // ---- 输入模式 ----
            switch (key) {
                case KEY_ENTER: {
                    std::string text = select_panel.get_input_text();
                    if (!text.empty()) {
                        select_panel.dismiss();
                        term->end_overlay();
                        scr->reset_buffers();
                        return text;
                    }
                    break;
                }
                case KEY_ESC:
                case KEY_CTRL_C:
                    select_panel.deactivate_input_mode();
                    break;
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
                case KEY_TAB:   select_panel.next_tab(); break;
                case KEY_SPACE: select_panel.toggle_current(); break;
                case KEY_ENTER: {
                    std::string chosen;
                    auto selected_ids = select_panel.get_selected_ids();
                    if (!selected_ids.empty()) {
                        chosen = selected_ids[0];
                    } else {
                        if (auto* cur = select_panel.get_current_item()) {
                            chosen = cur->id;
                        }
                    }
                    if (chosen == CUSTOM_MODEL_ID) {
                        select_panel.activate_input_mode();
                        break;
                    }
                    if (!chosen.empty()) {
                        select_panel.dismiss();
                        term->end_overlay();
                        scr->reset_buffers();
                        return chosen;
                    }
                    break;
                }
                case 'q': case 'Q': case KEY_ESC: case KEY_CTRL_C:
                    select_panel.dismiss();
                    term->end_overlay();
                    scr->reset_buffers();
                    return "";
                default: break;
            }
        }
    }
}

} // namespace workx
