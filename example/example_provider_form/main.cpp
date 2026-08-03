/**
 * @file main.cpp
 * @brief 供应商多字段表单 TUI 验证示例（example_provider_form）
 * @details 重新设计的供应商配置 UI：不再逐步向导，而是单屏表单直接显示全部字段。
 *          - 每个输入框都有独立边框（╭─ 标题 ─╮ / │ 值 │ / ╰─────╯）
 *          - 字段：供应商名称 / Base URL / 模型 ID / 上下文窗口 / API Key
 *          - 供应商名称框聚焦时自动弹出提示列表（对齐命令面板 / 风格）：
 *            前缀实时过滤内置供应商预设 + Custom URL（自定义，非内置）
 *          - Enter 依次切换到下一个字段（循环）；提示可见时 Enter 填入选中项
 *          - 输入模型 ID 时实时从 models.dev catalog + 静态能力表匹配上下文窗口并自动填充
 *          - 多供应商管理：Tab 切换、N 新建、D 删除、Ctrl+S 保存、Esc 退出
 * @usage   example_provider_form [providers.json]   # 可加载已有供应商文件
 */

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "agent/model/config.h"
#include "agent/model/model_catalog.h"
#include "agent/model/provider_preset.h"
#include "app/config/app_config.h"
#include "app/factory.h"
#include "core/config/config_manager.h"
#include "core/events/event_bus.h"
#include "core/task/task_manager.h"
#include "tui/core/platform/i_platform.h"
#include "tui/core/screen.h"
#include "tui/core/terminal.h"
#include "tui/utils/utf8_utils.h"

using namespace tui;
using namespace agent;

namespace {

// 特殊按键码（与 session_picker / model_selector 一致）
constexpr char32_t KEY_UP       = 0xE002;
constexpr char32_t KEY_DOWN     = 0xE003;
constexpr char32_t KEY_LEFT     = 0xE004;
constexpr char32_t KEY_RIGHT    = 0xE005;
constexpr char32_t KEY_ENTER    = 0x0D;
constexpr char32_t KEY_ESC      = 0x1B;
constexpr char32_t KEY_TAB      = 0x09;
constexpr char32_t KEY_CTRL_C   = 0xE009;
constexpr char32_t KEY_CTRL_S   = 0x13;
constexpr char32_t KEY_CTRL_N   = 0xE00C;
constexpr char32_t KEY_CTRL_D   = 0xE00D;
constexpr char32_t KEY_BACKSPACE = 0x08;
constexpr char32_t KEY_DELETE   = 0x7F;

/// 将 Unicode codepoint 追加为 UTF-8（支持中文等非 ASCII 输入）
void append_utf8_cp(char32_t cp, std::string& out) {
    if (cp <= 0x7F) {
        out += static_cast<char>(cp);
    } else if (cp <= 0x7FF) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp <= 0xFFFF) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

/// 删除字符串末尾一个完整 UTF-8 字符
void pop_utf8_back(std::string& s) {
    if (s.empty()) return;
    size_t i = s.size() - 1;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
    s.erase(i);
}

// 表单字段
enum Field : int {
    F_NAME = 0,     // 供应商名称
    F_URL,          // Base URL
    F_MODEL,        // 模型 ID
    F_CONTEXT,      // 上下文窗口
    F_KEY,          // API Key
    F_COUNT
};

/// 单个供应商条目
struct ProviderEntry {
    std::string name;           // 供应商名称
    std::string base_url;       // Base URL
    std::string model;          // 模型 ID
    int32_t context_length = 0; // 上下文窗口（token），0 = 未知
    std::string api_key;        // API Key
};

/// 表单状态
struct FormState {
    std::vector<ProviderEntry> providers;
    int active = 0;             // 当前编辑的供应商索引
    Field field = F_NAME;       // 当前聚焦字段
    bool context_auto = true;   // 上下文窗口是否由模型匹配自动填充
    bool dirty = false;         // 有未保存修改
};

/// 供应商提示条目（命令面板风格）
struct SuggestItem {
    std::string name;      // 预设内部名（如 "deepseek"；custom 为 "openai-compatible"）
    std::string display;   // 显示名（如 "DeepSeek"）
    std::string desc;      // 描述（如默认 URL）
    bool is_custom = false; // 是否为 Custom URL（非内置）
};

/// 模型 ID → 上下文窗口自动匹配（catalog 优先，静态能力表兜底）
int32_t match_context_window(const ModelCatalog* catalog, std::string_view model) {
    if (model.empty()) return 0;
    if (catalog) {
        int32_t v = catalog->context_window_for(model);
        if (v > 0) return v;
    }
    const ModelCapability* cap = find_model_capability(model);
    if (cap && cap->context_window > 0) return cap->context_window;
    return 0;
}

/// 内置预设 + Custom URL → 提示列表
std::vector<SuggestItem> build_suggestions() {
    std::vector<SuggestItem> out;
    for (auto name : list_preset_names()) {
        const ProviderPreset* p = find_preset(name);
        if (!p) continue;
        SuggestItem item;
        item.name = std::string(p->name);
        item.display = p->display_name;
        item.desc = p->default_url.empty()
            ? std::string(p->name) + " 协议" : p->default_url;
        item.is_custom = (p->name == "openai-compatible");
        out.push_back(std::move(item));
    }
    return out;
}

/// 前缀过滤提示列表（不区分大小写，匹配 display / name）
std::vector<SuggestItem> filter_suggestions(
    const std::vector<SuggestItem>& all, const std::string& prefix) {
    std::vector<SuggestItem> out;
    auto ic_startswith = [](const std::string& s, const std::string& p) {
        if (p.size() > s.size()) return false;
        for (size_t i = 0; i < p.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(s[i])) !=
                std::tolower(static_cast<unsigned char>(p[i]))) {
                return false;
            }
        }
        return true;
    };
    for (const auto& item : all) {
        if (prefix.empty() ||
            ic_startswith(item.display, prefix) ||
            ic_startswith(item.name, prefix)) {
            out.push_back(item);
        }
    }
    return out;
}

/// 内置预设 → 初始供应商列表
std::vector<ProviderEntry> default_providers() {
    std::vector<ProviderEntry> out;
    for (auto name : list_preset_names()) {
        if (name == "openai-compatible") continue;
        const ProviderPreset* p = find_preset(name);
        if (!p) continue;
        ProviderEntry e;
        e.name = p->display_name;
        e.base_url = p->default_url;
        e.model = p->default_model;
        e.context_length = p->default_context_length;
        out.push_back(std::move(e));
    }
    return out;
}

/// 从文件加载供应商列表（失败返回空）
std::vector<ProviderEntry> load_providers(const std::filesystem::path& path) {
    std::vector<ProviderEntry> out;
    std::ifstream ifs(path);
    if (!ifs) return out;
    try {
        auto j = nlohmann::json::parse(ifs);
        if (!j.contains("providers")) return out;
        for (const auto& item : j["providers"]) {
            ProviderEntry e;
            e.name = item.value("name", "");
            e.base_url = item.value("base_url", "");
            e.model = item.value("model", "");
            e.context_length = item.value("context_length", 0);
            e.api_key = item.value("api_key", "");
            out.push_back(std::move(e));
        }
    } catch (...) {
        return out;
    }
    return out;
}

/// 保存供应商列表到文件
void save_providers(const std::filesystem::path& path, const FormState& st) {
    nlohmann::json j;
    j["active"] = st.active;
    j["providers"] = nlohmann::json::array();
    for (const auto& e : st.providers) {
        j["providers"].push_back({
            {"name", e.name},
            {"base_url", e.base_url},
            {"model", e.model},
            {"context_length", e.context_length},
            {"api_key", e.api_key}
        });
    }
    std::ofstream ofs(path);
    ofs << j.dump(2);
}

/// 当前字段值引用（F_CONTEXT 不走此函数，由调用方单独处理）
std::string& field_value(ProviderEntry& e, Field f) {
    switch (f) {
        case F_NAME:    return e.name;
        case F_URL:     return e.base_url;
        case F_MODEL:   return e.model;
        case F_KEY:     return e.api_key;
        default: {
            static std::string s_dummy;
            return s_dummy;
        }
    }
}

/// 字段标签（框标题）
std::string field_label(Field f) {
    switch (f) {
        case F_NAME:    return "供应商名称";
        case F_URL:     return "Base URL";
        case F_MODEL:   return "模型 ID";
        case F_CONTEXT: return "上下文窗口";
        case F_KEY:     return "API Key";
        default:        return "";
    }
}

/// 截断字符串到指定显示宽度
std::string truncate_to_width(const std::string& s, int max_cols) {
    if (display_width(s) <= max_cols) return s;
    std::string out;
    int w = 0;
    for (size_t i = 0; i < s.size() && w < max_cols - 1;) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t len = 1;
        if ((c & 0xE0) == 0xC0) len = 2;
        else if ((c & 0xF0) == 0xE0) len = 3;
        else if ((c & 0xF8) == 0xF0) len = 4;
        if (i + len > s.size()) break;
        out += s.substr(i, len);
        w += display_width(s.substr(i, len));
        i += len;
    }
    out += "…";
    return out;
}

/// API Key 掩码显示
std::string mask_key(const std::string& key) {
    if (key.empty()) return "(空)";
    if (key.size() <= 8) return "****";
    return key.substr(0, 4) + "****";
}

} // namespace

// ============================================================================
// 主函数
// ============================================================================

int main(int argc, char* argv[]) {
    // ---- 加载供应商（参数指定文件，否则用内置预设） ----
    FormState st;
    if (argc >= 2) {
        st.providers = load_providers(argv[1]);
        if (st.providers.empty()) {
            std::cerr << "[provider_form] 无法加载 " << argv[1] << "，使用内置预设\n";
            st.providers = default_providers();
        }
    } else {
        st.providers = default_providers();
    }

    // ---- 初始化 Terminal + Screen（对齐 choice_preview） ----
    Terminal terminal(&EventBus::instance(), &ConfigManager::instance(),
                      &TaskManager::instance(),
                      make_terminal_config(ConfigManager::instance()));
    auto init_result = terminal.initialize();
    if (init_result.isErr()) {
        std::cerr << "[provider_form] Terminal 初始化失败: " << init_result.error() << "\n";
        terminal.restore();
        return 1;
    }
    Screen screen(&terminal);
    screen.resize(terminal.get_terminal_width(), terminal.get_terminal_height());

    // ---- 尝试加载 models.dev catalog（可选，用于模型 ID → 上下文窗口自动匹配） ----
    std::shared_ptr<const ModelCatalog> catalog;
    auto cache_path = default_config_path().parent_path() / "models_cache.json";
    if (auto r = ModelCatalog::load_cache(cache_path); r.is_ok()) {
        catalog = std::make_shared<const ModelCatalog>(std::move(r.value()));
    }

    // ---- 提示列表 ----
    const std::vector<SuggestItem> all_suggestions = build_suggestions();
    std::vector<SuggestItem> filtered_suggestions;
    int suggest_selected = 0;

    int overlay_bottom = terminal.get_terminal_height() - 1;
    if (overlay_bottom < 1) overlay_bottom = 1;
    terminal.begin_overlay(1, overlay_bottom);

    std::string status_msg;
    bool quit = false;
    bool esc_armed = false;  // dirty 时第一次 Esc 提示，第二次确认退出

    // ---- 两层 UI：列表层（第一层）+ 表单层（第二层） ----
    enum class Layer { List, Form };
    Layer layer = Layer::List;
    int list_cursor = 0;             // 列表层选中项（avail_indices 内下标）
    bool editing_new = false;        // 表单层是否处于"新增"流程（区别保存提示文案）
    std::vector<int> avail_indices;  // 可用配置在 st.providers 中的原始索引

    // 重建可用配置索引（name/base_url/model 均非空才算可用）
    auto rebuild_avail = [&]() {
        avail_indices.clear();
        for (int i = 0; i < static_cast<int>(st.providers.size()); ++i) {
            const auto& e = st.providers[i];
            if (!e.name.empty() && !e.base_url.empty() && !e.model.empty()) {
                avail_indices.push_back(i);
            }
        }
        if (list_cursor >= static_cast<int>(avail_indices.size())) {
            list_cursor = static_cast<int>(avail_indices.size()) - 1;
        }
        if (list_cursor < 0) list_cursor = 0;
    };

    // 名称框内容变化 → 刷新提示过滤
    auto refresh_suggestions = [&]() {
        filtered_suggestions = filter_suggestions(all_suggestions, st.providers[st.active].name);
        if (suggest_selected >= static_cast<int>(filtered_suggestions.size())) {
            suggest_selected = static_cast<int>(filtered_suggestions.size()) - 1;
        }
        if (suggest_selected < 0) suggest_selected = 0;
    };

    // ==================== 渲染 ====================
    auto do_render = [&]() {
        screen.clear();
        int width = screen.width();
        if (width < 50) width = 50;

        int row = 0;
        const std::string HORIZ = "─", VERT = "│", TL = "╭", TR = "╮", BL = "╰", BR = "╯";

        if (layer == Layer::List) {
            // ============ 第一层：配置列表 ============
            std::string title = std::format("  Provider 配置  {} 可用 / {} 总",
                                            avail_indices.size(), st.providers.size());
            screen.write(row, 0, title, ColorRole::StatusBar);
            row += 2;

            // 每项 4 行（顶/模型/URL/底），最多同时显示 3 项，选中项保持可见
            const int MAX_VISIBLE = 3;
            int n = static_cast<int>(avail_indices.size());
            int scroll = list_cursor - 1;
            if (scroll < 0) scroll = 0;
            if (scroll > std::max(0, n - MAX_VISIBLE)) scroll = std::max(0, n - MAX_VISIBLE);
            int visible = std::min(MAX_VISIBLE, n - scroll);

            auto draw_list_box = [&](const ProviderEntry& e, bool is_active, bool is_sel) {
                ColorRole border = is_active ? ColorRole::Success   // 使用中：绿色边框
                                 : is_sel   ? ColorRole::StatusBar  // 选中：白色边框
                                 : ColorRole::Dim;                  // 其他：灰色
                // 顶边框
                std::string top = "  " + TL + HORIZ + " " + e.name + " ";
                if (is_active) top += "● 使用中 ";
                if (is_sel) top += "»";
                int pad = width - display_width(top) - 1;
                if (pad < 1) pad = 1;
                for (int i = 0; i < pad; ++i) top += HORIZ;
                top += TR;
                screen.write(row, 0, top, border);
                row++;
                // 内容行 1：模型
                std::string m = "  " + VERT + "  模型: " + (e.model.empty() ? "(空)" : e.model);
                int fill = width - display_width(m) - 1;
                if (fill < 0) fill = 0;
                m += std::string(static_cast<size_t>(fill), ' ') + VERT;
                screen.write(row, 0, m, ColorRole::Default);
                row++;
                // 内容行 2：URL + 上下文 + key
                std::string ctx = e.context_length > 0 ? std::to_string(e.context_length) : "(未知)";
                std::string key = e.api_key.empty() ? "(无)" : mask_key(e.api_key);
                std::string u = "  " + VERT + "  " + (e.base_url.empty() ? "(空)" : e.base_url) +
                                "  ·  ctx " + ctx + "  ·  " + key;
                u = truncate_to_width(u, width - 3);
                fill = width - display_width(u) - 1;
                if (fill < 0) fill = 0;
                u += std::string(static_cast<size_t>(fill), ' ') + VERT;
                screen.write(row, 0, u, ColorRole::Dim);
                row++;
                // 底边框
                std::string bottom = "  " + BL;
                for (int i = 0; i < width - 4; ++i) bottom += HORIZ;
                bottom += BR;
                screen.write(row, 0, bottom, border);
                row++;
            };

            for (int v = 0; v < visible; ++v) {
                int idx = avail_indices[scroll + v];
                draw_list_box(st.providers[idx], idx == st.active, scroll + v == list_cursor);
                row++;  // 项间距
            }

            if (n == 0) {
                screen.write(row, 0, "  暂无可用配置，按 Ctrl+N 新增", ColorRole::Dim);
                row++;
            }

            // 状态 / 提示
            if (!status_msg.empty()) {
                screen.write(row, 0, "  " + status_msg, ColorRole::Success);
                row++;
            }
            std::string footer = "  ↑↓ 选择 · Enter 编辑 · Tab/空格 设为使用中 · Ctrl+N 新增 · Ctrl+D 删除 · Esc 退出";
            if (static_cast<int>(display_width(footer)) > width) footer = truncate_to_width(footer, width);
            screen.write(row, 0, footer, ColorRole::Dim);
        } else {
        // 标题行
        std::string title = std::format("  Provider  {}/{}", st.active + 1, st.providers.size());
        if (st.dirty) title += "  *";
        screen.write(row, 0, title, ColorRole::StatusBar);
        row++;

        ProviderEntry& cur = st.providers[st.active];

        // 绘制一个带标题边框的输入框（3 行）
        auto draw_field_box = [&](const std::string& label, const std::string& value,
                                  bool focused, const std::string& hint = "") {
            // 上边框：  ╭─ 标题 ─────────╮
            std::string top = "  " + TL + HORIZ + " " + label + " ";
            int pad = width - display_width(top) - 1;
            if (pad < 1) pad = 1;
            for (int i = 0; i < pad; ++i) top += HORIZ;
            top += TR;
            ColorRole border_color = focused ? ColorRole::StatusBar : ColorRole::Dim;
            screen.write(row, 0, top, border_color);
            row++;

            // 值行：  │ value  hint        │
            int right_col = width - 1;
            std::string content;
            if (!hint.empty()) {
                int hint_room = right_col - display_width("  " + VERT + " " + value) - 2;
                if (hint_room >= 2) {
                    std::string h = truncate_to_width(hint, hint_room);
                    content = "  " + VERT + " " + value +
                              std::string(static_cast<size_t>(
                                  right_col - display_width("  " + VERT + " " + value) - display_width(h) - 1),
                                  ' ') +
                              h + " " + VERT;
                } else {
                    content = "  " + VERT + " " + value;
                }
            } else {
                content = "  " + VERT + " " + value;
            }
            int used = display_width(content);
            int fill = right_col - used;
            if (fill < 0) fill = 0;
            content += std::string(static_cast<size_t>(fill), ' ') + VERT;
            ColorRole value_color = focused ? ColorRole::StatusBar : ColorRole::Default;
            screen.write(row, 0, content, value_color);
            row++;

            // 下边框：  ╰────────────────╯
            std::string bottom = "  " + BL;
            for (int i = 0; i < width - 4; ++i) bottom += HORIZ;
            bottom += BR;
            screen.write(row, 0, bottom, border_color);
            row++;
        };

        // 提示列表（命令面板风格，仅名称框聚焦时渲染）
        bool show_suggest = (st.field == F_NAME && !filtered_suggestions.empty());
        auto draw_suggest_box = [&]() {
            const int MAX_ITEMS = 5;
            int display_count = std::min(MAX_ITEMS, static_cast<int>(filtered_suggestions.size()));
            // 滚动窗口：选中项保持可见
            int scroll = 0;
            if (suggest_selected >= display_count) scroll = suggest_selected - display_count + 1;

            // 上边框：  ╭─ 匹配 ─────────╮
            std::string top = "  " + TL + HORIZ + " 匹配 ";
            int pad = (width - 1) - display_width(top) - 1;
            if (pad < 1) pad = 1;
            for (int i = 0; i < pad; ++i) top += HORIZ;
            top += TR;
            screen.write(row, 0, top, ColorRole::Dim);
            row++;

            int right_col = width - 1;
            for (int i = 0; i < display_count; ++i) {
                const auto& item = filtered_suggestions[scroll + i];
                bool sel = (scroll + i == suggest_selected);
                std::string marker = sel ? "» " : "  ";
                std::string line = marker + item.display;
                if (!item.desc.empty()) line += "  " + item.desc;
                if (item.is_custom) line += "  (自定义地址)";
                line = truncate_to_width(line, right_col - 3);
                std::string content = "  " + VERT + " " + line;
                int fill = right_col - display_width(content);
                if (fill < 0) fill = 0;
                content += std::string(static_cast<size_t>(fill), ' ') + VERT;
                ColorRole c = sel ? ColorRole::CommandPanelHighlight : ColorRole::Default;
                screen.write(row, 0, content, c);
                row++;
            }

            // 下边框
            std::string bottom = "  " + BL;
            for (int i = 0; i < width - 4; ++i) bottom += HORIZ;
            bottom += BR;
            screen.write(row, 0, bottom, ColorRole::Dim);
            row++;
        };

        // ---- 字段值计算 ----
        auto field_display = [&](Field f) -> std::string {
            if (f == F_CONTEXT) {
                int32_t auto_ctx = match_context_window(catalog.get(), cur.model);
                if (st.context_auto && auto_ctx > 0) {
                    return std::format("{} (auto)", auto_ctx);
                }
                if (cur.context_length > 0) {
                    return std::to_string(cur.context_length) +
                           (st.context_auto ? " (auto)" : " (manual)");
                }
                return "(未匹配，可直接输入数字)";
            }
            std::string v = field_value(cur, f);
            if (f == F_KEY && !v.empty() && st.field != f) v = mask_key(v);
            if (v.empty()) v = "(空)";
            return v;
        };

        // ---- 渲染 5 个输入框 ----
        draw_field_box(field_label(F_NAME), field_display(F_NAME), st.field == F_NAME,
                       st.field == F_NAME && !filtered_suggestions.empty()
                           ? std::format("{} 个匹配，↑↓ 选择", filtered_suggestions.size())
                           : "");
        if (show_suggest) draw_suggest_box();
        draw_field_box(field_label(F_URL), field_display(F_URL), st.field == F_URL,
                       st.field == F_URL ? "自定义供应商请输入完整 URL" : "");
        draw_field_box(field_label(F_MODEL), field_display(F_MODEL), st.field == F_MODEL,
                       st.field == F_MODEL ? "输入模型 ID 自动匹配上下文窗口" : "");
        draw_field_box(field_label(F_CONTEXT), field_display(F_CONTEXT), st.field == F_CONTEXT,
                       st.field == F_CONTEXT ? "可直接输入数字覆盖" : "");
        draw_field_box(field_label(F_KEY), field_display(F_KEY), st.field == F_KEY,
                       st.field == F_KEY ? "粘贴 API Key 后按 Enter" : "");

        // ---- 状态 / 提示 ----
        if (!status_msg.empty()) {
            screen.write(row, 0, "  " + status_msg, ColorRole::Success);
            row++;
        }
        std::string footer = "  Enter 下一字段 · ↑↓ 提示选择/字段 · Tab 切换 · Ctrl+S 保存 · Esc 返回";
        if (static_cast<int>(display_width(footer)) > width) footer = truncate_to_width(footer, width);
        screen.write(row, 0, footer, ColorRole::Dim);
        }  // else 表单层

        screen.flush();
    };

    refresh_suggestions();

    // ==================== 交互循环 ====================
    auto save_all = [&](std::string_view msg) {
        std::filesystem::path save_path = (argc >= 2)
            ? std::filesystem::path(argv[1])
            : std::filesystem::path("providers.json");
        save_providers(save_path, st);
        st.dirty = false;
        esc_armed = false;
        status_msg = std::format("{} {}", msg, save_path.string());
    };

    rebuild_avail();

    while (!quit) {
        do_render();
        char32_t key = terminal.platform()->read_char();
        status_msg.clear();

        // ============ 第一层：配置列表 ============
        if (layer == Layer::List) {
            if (key == KEY_UP || key == KEY_DOWN) {
                int n = static_cast<int>(avail_indices.size());
                if (n > 0) {
                    if (key == KEY_UP) {
                        list_cursor = (list_cursor <= 0) ? n - 1 : list_cursor - 1;
                    } else {
                        list_cursor = (list_cursor >= n - 1) ? 0 : list_cursor + 1;
                    }
                }
            } else if (key == KEY_ENTER) {
                if (!avail_indices.empty()) {
                    // 编辑选中配置 → 进入表单层（预填）
                    st.active = avail_indices[list_cursor];
                    st.field = F_NAME;
                    st.context_auto = true;
                    st.dirty = false;
                    esc_armed = false;
                    editing_new = false;
                    refresh_suggestions();
                    layer = Layer::Form;
                    status_msg = std::format("编辑配置：{}", st.providers[st.active].name);
                } else {
                    status_msg = "暂无可用配置，按 Ctrl+N 新增";
                }
            } else if (key == KEY_TAB || key == ' ') {
                if (!avail_indices.empty()) {
                    // 设为使用中（绿色边框项）
                    st.active = avail_indices[list_cursor];
                    save_all("已设为使用中:");
                } else {
                    status_msg = "暂无可用配置，按 Ctrl+N 新增";
                }
            } else if (key == KEY_CTRL_N) {
                // 新增配置 → 进入表单层
                ProviderEntry ne;
                ne.name = "New Provider";
                st.providers.push_back(std::move(ne));
                st.active = static_cast<int>(st.providers.size()) - 1;
                st.field = F_NAME;
                st.context_auto = true;
                st.dirty = true;
                esc_armed = false;
                editing_new = true;
                refresh_suggestions();
                layer = Layer::Form;
                status_msg = "新增配置：输入名称（可参考提示）";
            } else if (key == KEY_CTRL_D) {
                if (st.providers.size() <= 1) {
                    status_msg = "至少保留一个配置";
                } else if (!avail_indices.empty()) {
                    int idx = avail_indices[list_cursor];
                    st.providers.erase(st.providers.begin() + idx);
                    if (st.active == idx) {
                        st.active = 0;
                    } else if (st.active > idx) {
                        --st.active;
                    }
                    st.dirty = true;
                    rebuild_avail();
                    status_msg = "已删除配置";
                } else {
                    status_msg = "当前无可删除的可用配置";
                }
                esc_armed = false;
            } else if (key == KEY_CTRL_S) {
                save_all("已保存到");
                rebuild_avail();
            } else if (key == KEY_ESC || key == KEY_CTRL_C) {
                quit = true;
            }
            continue;
        }

        // ============ 第二层：表单 ============
        ProviderEntry& cur = st.providers[st.active];
        bool suggest_visible = (st.field == F_NAME && !filtered_suggestions.empty());

        if (key == KEY_ENTER) {
            if (suggest_visible) {
                // 提示可见：填入选中项
                const auto& item = filtered_suggestions[suggest_selected];
                if (item.is_custom) {
                    cur.name = "Custom URL";
                    cur.base_url.clear();
                    cur.model.clear();
                    cur.context_length = 0;
                    st.context_auto = true;
                    status_msg = "已选择自定义，请输入 Base URL";
                } else {
                    const ProviderPreset* p = find_preset(item.name);
                    if (p) {
                        cur.name = p->display_name;
                        cur.base_url = p->default_url;
                        cur.model = p->default_model;
                        cur.context_length = p->default_context_length;
                        st.context_auto = true;
                        status_msg = std::format("已填入 {} 预设", p->display_name);
                    }
                }
                st.dirty = true;
                st.field = F_URL;  // 填入后进入下一字段
                filtered_suggestions.clear();
            } else if (st.field == F_KEY) {
                // 最后一个字段 Enter = 保存：返回配置列表
                save_all(editing_new
                             ? std::format("已添加供应商 {}", cur.name)
                             : std::format("已保存供应商 {}", cur.name));
                rebuild_avail();
                layer = Layer::List;
            } else {
                // 依次切换到下一个字段（循环）
                st.field = static_cast<Field>((static_cast<int>(st.field) + 1) % F_COUNT);
                if (st.field == F_NAME) status_msg = "已循环到第一个字段";
            }
            esc_armed = false;
        } else if (key == KEY_UP || key == KEY_DOWN) {
            if (suggest_visible) {
                // 提示列表内移动（对齐命令面板）
                int n = static_cast<int>(filtered_suggestions.size());
                if (key == KEY_UP) {
                    suggest_selected = (suggest_selected <= 0) ? n - 1 : suggest_selected - 1;
                } else {
                    suggest_selected = (suggest_selected >= n - 1) ? 0 : suggest_selected + 1;
                }
            } else {
                st.field = static_cast<Field>((static_cast<int>(st.field) + (key == KEY_UP ? F_COUNT - 1 : 1)) % F_COUNT);
            }
            esc_armed = false;
        } else if (key == KEY_TAB) {
            if (suggest_visible) {
                // Tab 补全（对齐命令面板）：填入名称，保留提示继续过滤
                // 注意：先拷贝 display，refresh_suggestions 会使 filtered_suggestions 的引用失效
                const auto& item = filtered_suggestions[suggest_selected];
                std::string display_copy = item.display;
                cur.name = display_copy;
                st.dirty = true;
                refresh_suggestions();
                status_msg = std::format("补全: {}", display_copy);
            } else {
                // 切换下一个供应商
                st.active = (st.active + 1) % static_cast<int>(st.providers.size());
                st.dirty = true;
                refresh_suggestions();
            }
            esc_armed = false;
        } else if (key == KEY_CTRL_S) {
            save_all("已保存到");
            rebuild_avail();
            layer = Layer::List;
        } else if (key == KEY_ESC || key == KEY_CTRL_C) {
            if (st.dirty && !esc_armed) {
                esc_armed = true;
                status_msg = "有未保存修改，再按一次 Esc 返回列表（不保存）";
                continue;
            }
            // 返回配置列表（不退出程序）
            rebuild_avail();
            layer = Layer::List;
            esc_armed = false;
            status_msg = "已返回配置列表";
        } else if (key == KEY_BACKSPACE || key == KEY_DELETE) {
            if (st.field == F_CONTEXT) {
                cur.context_length /= 10;
                if (cur.context_length == 0) st.context_auto = true;
            } else {
                std::string& v = field_value(cur, st.field);
                if (!v.empty()) {
                    pop_utf8_back(v);
                    st.dirty = true;
                    esc_armed = false;
                    if (st.field == F_MODEL) {
                        cur.context_length = match_context_window(catalog.get(), cur.model);
                        st.context_auto = (cur.context_length > 0);
                    } else if (st.field == F_NAME) {
                        refresh_suggestions();
                    }
                }
            }
        } else if (key >= '0' && key <= '9' && st.field == F_CONTEXT) {
            if (cur.context_length == 0) st.context_auto = false;
            cur.context_length = cur.context_length * 10 + (key - '0');
            st.context_auto = false;
            st.dirty = true;
            esc_armed = false;
        } else if (key >= 0x20 && key < 0xE000) {  // 可打印字符（含中文），0xE000+ 为特殊键区
            std::string& v = field_value(cur, st.field);
            if (v.size() < 512) {
                append_utf8_cp(key, v);
                st.dirty = true;
                esc_armed = false;
                if (st.field == F_MODEL) {
                    cur.context_length = match_context_window(catalog.get(), cur.model);
                    st.context_auto = (cur.context_length > 0);
                } else if (st.field == F_NAME) {
                    refresh_suggestions();
                }
            }
        }
        // 其他键忽略
    }

    terminal.end_overlay();
    screen.reset_buffers();
    terminal.restore();

    // ---- 输出结果 ----
    std::cout << "\n========== 供应商配置结果 ==========\n";
    for (size_t i = 0; i < st.providers.size(); ++i) {
        const auto& e = st.providers[i];
        std::cout << (static_cast<int>(i) == st.active ? "▶ " : "  ")
                  << e.name << "\n"
                  << "    base_url : " << (e.base_url.empty() ? "(空)" : e.base_url) << "\n"
                  << "    model    : " << (e.model.empty() ? "(空)" : e.model) << "\n"
                  << "    context  : " << (e.context_length > 0 ? std::to_string(e.context_length) : "(未知)") << "\n"
                  << "    api_key  : " << (e.api_key.empty() ? "(空)" : "****" + e.api_key.substr(e.api_key.size() > 4 ? e.api_key.size() - 4 : e.api_key.size())) << "\n";
    }
    std::cout << "=====================================\n";
    return 0;
}
