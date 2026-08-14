/**
 * @file provider_form.cpp
 * @brief 交互式供应商管理实现（两层 TUI：配置列表 + 字段表单）
 * @details 第一层列出可用配置（name/base_url/model 均非空），绿色边框=使用中、
 *          白色边框=选中；Enter 编辑、Tab/空格 设为使用中、Ctrl+N 新增、
 *          Ctrl+D 删除。第二层为 5 字段表单（名称/URL/模型/上下文/Key），
 *          名称框带命令面板风格提示列表（内置预设 + Custom URL）。
 *          多供应商列表经 ConfigManager 持久化（backend.providers JSON 数组）。
 * @version 2.0.0
 * @date 2026-08
 */

#include "app/ui/provider_form.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <vector>

#include "agent/model/config.h"
#include "agent/model/model_catalog.h"
#include "agent/model/provider_preset.h"
#include "agent/config/app_config.h"
#include "core/config/i_config_manager.h"
#include "tui/core/color_scheme.h"
#include "tui/core/platform/i_platform.h"
#include "tui/core/screen.h"
#include "tui/core/terminal.h"
#include "tui/utils/utf8_utils.h"

namespace agent {

using namespace tui;

namespace {

// 特殊按键码（与 session_picker / model_selector 一致）
constexpr char32_t KEY_UP        = 0xE002;
constexpr char32_t KEY_DOWN      = 0xE003;
constexpr char32_t KEY_ENTER     = 0x0D;
constexpr char32_t KEY_ESC       = 0x1B;
constexpr char32_t KEY_TAB       = 0x09;
constexpr char32_t KEY_CTRL_C    = 0xE009;
constexpr char32_t KEY_CTRL_S    = 0x13;
constexpr char32_t KEY_CTRL_N    = 0xE011;  // H-2（PR #46）：0xE00C 让位给 VT Backtab，Ctrl+N 改用 0xE011
constexpr char32_t KEY_CTRL_D    = 0xE00D;
constexpr char32_t KEY_BACKSPACE = 0x08;
constexpr char32_t KEY_DELETE    = 0x7F;
constexpr char32_t KEY_RESIZE    = 0xE00B;  // 终端尺寸变更（SIGWINCH → platform 返回）

/// Custom URL 预设内部名
constexpr const char* CUSTOM_PROVIDER_ID = "openai-compatible";

/// 表单字段
enum Field : int {
    F_NAME = 0,     // 供应商名称
    F_URL,          // Base URL
    F_MODEL,        // 模型 ID
    F_CONTEXT,      // 上下文窗口
    F_KEY,          // API Key
    F_COUNT
};

/// 表单状态
struct FormState {
    std::vector<ProviderConfigEntry> providers;
    int active = 0;             // 当前编辑的供应商索引
    Field field = F_NAME;       // 当前聚焦字段
    bool context_auto = true;   // 上下文窗口是否由模型匹配自动填充
    bool dirty = false;         // 有未保存修改（写回 ConfigManager）
};

/// 供应商提示条目（命令面板风格）
struct SuggestItem {
    std::string name;      // 预设内部名（如 "deepseek"；custom 为 "openai-compatible"）
    std::string display;   // 显示名（如 "DeepSeek"）
    std::string desc;      // 描述（如默认 URL）
    bool is_custom = false; // 是否为 Custom URL（非内置）
};

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

/// 截断字符串到指定显示宽度（按 UTF-8 字符边界）
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

/// 内置预设提示列表（custom 标记为自定义，置底）
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
        item.is_custom = (p->name == CUSTOM_PROVIDER_ID);
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
        if (ic_startswith(item.display, prefix) || ic_startswith(item.name, prefix)) {
            out.push_back(item);
        }
    }
    return out;
}

/// 模型 ID → 上下文窗口自动匹配（catalog 优先，静态能力表兜底）
int32_t match_context_window(const ModelCatalog* catalog, std::string_view model) {
    if (model.empty()) return 0;
    if (catalog) {
        int32_t v = catalog->context_window_for(model);
        if (v > 0) return v;
    }
    if (auto capability = find_model_capability(model)) {
        return capability->context_window;
    }
    return 0;
}

/// 内置预设生成默认配置列表（首次打开面板时使用）
std::vector<ProviderConfigEntry> default_providers() {
    std::vector<ProviderConfigEntry> out;
    for (auto name : list_preset_names()) {
        if (name == CUSTOM_PROVIDER_ID) continue;
        const ProviderPreset* p = find_preset(name);
        if (!p) continue;
        ProviderConfigEntry e;
        e.id = std::string(p->name);
        e.name = p->display_name;
        e.base_url = p->default_url;
        e.model = p->default_model;
        e.context_length = p->default_context_length;
        out.push_back(std::move(e));
    }
    return out;
}

/// 当前字段值指针（F_CONTEXT 无字符串存储，返回 nullptr，由调用方单独处理）
std::string* field_value(ProviderConfigEntry& e, Field f) {
    switch (f) {
        case F_NAME:    return &e.name;
        case F_URL:     return &e.base_url;
        case F_MODEL:   return &e.model;
        case F_KEY:     return &e.api_key;
        default:        return nullptr;
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

} // anonymous namespace

// ============================================================
// ConfigManager 持久化
// ============================================================

std::vector<ProviderConfigEntry> load_provider_configs(IConfigManager& cfg) {
    std::vector<ProviderConfigEntry> out;
    if (!cfg.has(keys::PROVIDERS)) return out;
    auto result = cfg.get<nlohmann::json>(keys::PROVIDERS);
    if (result.is_err()) return out;
    const auto& j = result.value();
    if (!j.is_array()) return out;
    for (const auto& item : j) {
        if (!item.is_object()) continue;
        ProviderConfigEntry e;
        e.id = item.value("id", "");
        e.name = item.value("name", "");
        e.base_url = item.value("base_url", "");
        e.model = item.value("model", "");
        e.context_length = item.value("context_length", 0);
        e.api_key = item.value("api_key", "");
        out.push_back(std::move(e));
    }
    return out;
}

void save_provider_configs(IConfigManager& cfg,
                           const std::vector<ProviderConfigEntry>& providers) {
    nlohmann::json j = nlohmann::json::array();
    for (const auto& e : providers) {
        j.push_back({
            {"id", e.id},
            {"name", e.name},
            {"base_url", e.base_url},
            {"model", e.model},
            {"context_length", e.context_length},
            {"api_key", e.api_key}
        });
    }
    cfg.set(keys::PROVIDERS, j);
}

void apply_provider_switch(IConfigManager& cfg, const ProviderConfigEntry& entry) {
    // 先清除旧键再写入新值：条目留空的字段不得残留上次供应商的配置。
    // （P1: 旧实现只 set 非空值，custom 预设全程空输入会残留旧 API_KEY/URL/MODEL_NAME）
    cfg.remove_value(keys::PROVIDER);
    cfg.remove_value(keys::REMOTE_URL);
    cfg.remove_value(keys::MODEL_NAME);
    cfg.remove_value(keys::API_KEY);

    cfg.set(keys::PROVIDER, entry.id.empty() ? entry.name : entry.id);
    if (!entry.base_url.empty()) {
        cfg.set(keys::REMOTE_URL, entry.base_url);
    }
    if (!entry.model.empty()) {
        cfg.set(keys::MODEL_NAME, entry.model);
    }
    if (!entry.api_key.empty()) {
        cfg.set(keys::API_KEY, entry.api_key);
    }
    // 上下文窗口：条目配置了显式值则写入标量（resolver 的 user cfg 级，启动/热切换均生效）；
    // 0 表示未设置，保持清除，让 catalog/静态表解析新供应商模型窗口。
    if (entry.context_length > 0) {
        cfg.set(keys::CONTEXT_LENGTH, static_cast<int>(entry.context_length));
    } else if (cfg.has(keys::CONTEXT_LENGTH)) {
        cfg.set(keys::CONTEXT_LENGTH, 0);
    }
}

// ============================================================
// 两层 TUI 面板
// ============================================================

ProviderSwitchResult provider_manager_interactive(
    IConfigManager& cfg,
    Terminal* term, Screen* scr)
{
    ProviderSwitchResult result;

    // ---- 加载供应商列表（首次使用内置预设） ----
    FormState st;
    st.providers = load_provider_configs(cfg);
    if (st.providers.empty()) {
        st.providers = default_providers();
    }
    // 当前使用中供应商（标量键 backend.provider），用于列表层绿色标记与初始定位
    std::string active_provider_id = cfg.get_or<std::string>(keys::PROVIDER, "");

    // ---- 初始化 Terminal + Screen ----
    int term_width = term->get_terminal_width();
    int term_height = term->get_terminal_height();
    int overlay_bottom = term_height - 1;
    if (overlay_bottom < 1) overlay_bottom = 1;
    term->begin_overlay(1, overlay_bottom);

    // 差分渲染对齐终端实际尺寸（Screen 默认 80x24，不 resize 会渲染截断/越界）
    scr->resize(term_width, term_height);
    // 清空 overlay 区域：差分渲染首帧不会输出"空白行"（m_previous 同为空白），
    // 若不显式清行会残留旧聊天内容（"没有清屏"）；结束后由 end_overlay() 从快照恢复
    for (int r = 1; r <= overlay_bottom; ++r) {
        char pos_cmd[32];
        snprintf(pos_cmd, sizeof(pos_cmd), "\x1b[%d;1H\x1b[2K", r);
        term->write(pos_cmd);
    }
    term->write("\x1b[1;1H");  // 光标归位 overlay 顶部，等待 Screen 差分渲染

    // ---- models.dev catalog（可选，用于模型 ID → 上下文窗口自动匹配） ----
    auto cache_path = default_config_path().parent_path() / "models_cache.json";
    std::shared_ptr<const ModelCatalog> catalog;
    if (auto r = ModelCatalog::load_cache(cache_path); r.is_ok()) {
        catalog = std::make_shared<const ModelCatalog>(std::move(r.value()));
    }

    // ---- 提示列表 ----
    const std::vector<SuggestItem> all_suggestions = build_suggestions();
    std::vector<SuggestItem> filtered_suggestions;
    int suggest_selected = 0;

    std::string status_msg;
    bool quit = false;
    bool esc_armed = false;

    // 进入表单层时对当前条目做快照，Esc 放弃时回滚编辑（不落盘也不残留内存）
    ProviderConfigEntry entry_snapshot;

    // ---- 两层 UI：列表层（第一层）+ 表单层（第二层） ----
    enum class Layer { List, Form };
    Layer layer = Layer::List;
    int list_cursor = -1;            // 列表层选中项（avail_indices 内下标，-1 表示未定位）
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
        if (list_cursor < 0) {
            // 初始定位：选中当前使用中的供应商
            list_cursor = 0;
            for (int v = 0; v < static_cast<int>(avail_indices.size()); ++v) {
                if (st.providers[avail_indices[v]].id == active_provider_id) {
                    list_cursor = v;
                    break;
                }
            }
        }
    };

    // 名称框内容变化 → 刷新提示过滤
    auto refresh_suggestions = [&]() {
        filtered_suggestions = filter_suggestions(all_suggestions, st.providers[st.active].name);
        if (suggest_selected >= static_cast<int>(filtered_suggestions.size())) {
            suggest_selected = static_cast<int>(filtered_suggestions.size()) - 1;
        }
        if (suggest_selected < 0) suggest_selected = 0;
    };

    // 保存列表到 ConfigManager（不落盘，由调用方统一 save_to_file）
    auto persist_list = [&]() {
        save_provider_configs(cfg, st.providers);
        st.dirty = false;
    };

    // ==================== 渲染 ====================
    auto do_render = [&]() {
        scr->clear();
        int width = scr->width();
        if (width < 50) width = 50;

        int row = 0;
        const std::string HORIZ = "─", VERT = "│", TL = "╭", TR = "╮", BL = "╰", BR = "╯";

        if (layer == Layer::List) {
            // ============ 第一层：配置列表 ============
            std::string title = std::format("  Provider 配置  {} 可用 / {} 总",
                                            avail_indices.size(), st.providers.size());
            if (st.dirty) title += "  *";
            scr->write(row, 0, title, ColorRole::StatusBar);
            row += 2;

            // 每项 4 行（顶/模型/URL/底），最多同时显示 3 项，选中项保持可见
            const int MAX_VISIBLE = 3;
            int n = static_cast<int>(avail_indices.size());
            int scroll = list_cursor - 1;
            if (scroll < 0) scroll = 0;
            if (scroll > std::max(0, n - MAX_VISIBLE)) scroll = std::max(0, n - MAX_VISIBLE);
            int visible = std::min(MAX_VISIBLE, n - scroll);

            auto draw_list_box = [&](const ProviderConfigEntry& e, bool is_active, bool is_sel) {
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
                scr->write(row, 0, top, border);
                row++;
                // 内容行 1：模型
                std::string m = "  " + VERT + "  模型: " + (e.model.empty() ? "(空)" : e.model);
                int fill = width - display_width(m) - 1;
                if (fill < 0) fill = 0;
                m += std::string(static_cast<size_t>(fill), ' ') + VERT;
                scr->write(row, 0, m, ColorRole::Default);
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
                scr->write(row, 0, u, ColorRole::Dim);
                row++;
                // 底边框
                std::string bottom = "  " + BL;
                for (int i = 0; i < width - 4; ++i) bottom += HORIZ;
                bottom += BR;
                scr->write(row, 0, bottom, border);
                row++;
            };

            for (int v = 0; v < visible; ++v) {
                int idx = avail_indices[scroll + v];
                draw_list_box(st.providers[idx],
                              st.providers[idx].id == active_provider_id,
                              scroll + v == list_cursor);
                row++;  // 项间距
            }

            if (n == 0) {
                scr->write(row, 0, "  暂无可用配置，按 Ctrl+N 新增", ColorRole::Dim);
                row++;
            }

            // 状态 / 提示
            if (!status_msg.empty()) {
                scr->write(row, 0, "  " + status_msg, ColorRole::Success);
                row++;
            }
            std::string footer = "  ↑↓ 选择 · Enter 编辑 · Tab/空格 设为使用中 · Ctrl+N 新增 · Ctrl+D 删除 · Esc 退出";
            if (static_cast<int>(display_width(footer)) > width) footer = truncate_to_width(footer, width);
            scr->write(row, 0, footer, ColorRole::Dim);
        } else {
        // ============ 第二层：表单 ============
        // 标题行
        std::string title = std::format("  编辑 Provider  {}/{}", st.active + 1, st.providers.size());
        if (st.dirty) title += "  *";
        scr->write(row, 0, title, ColorRole::StatusBar);
        row++;

        ProviderConfigEntry& cur = st.providers[st.active];

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
            scr->write(row, 0, top, border_color);
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
            scr->write(row, 0, content, value_color);
            row++;

            // 下边框：  ╰────────────────╯
            std::string bottom = "  " + BL;
            for (int i = 0; i < width - 4; ++i) bottom += HORIZ;
            bottom += BR;
            scr->write(row, 0, bottom, border_color);
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
            scr->write(row, 0, top, ColorRole::Dim);
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
                scr->write(row, 0, content, c);
                row++;
            }

            // 下边框
            std::string bottom = "  " + BL;
            for (int i = 0; i < width - 4; ++i) bottom += HORIZ;
            bottom += BR;
            scr->write(row, 0, bottom, ColorRole::Dim);
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
            std::string* vp = field_value(cur, f);
            std::string v = vp ? *vp : "";
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
            scr->write(row, 0, "  " + status_msg, ColorRole::Success);
            row++;
        }
        std::string footer = "  Enter 下一字段 · ↑↓ 提示选择/字段 · Tab 切换 · Ctrl+S 保存 · Esc 返回";
        if (static_cast<int>(display_width(footer)) > width) footer = truncate_to_width(footer, width);
        scr->write(row, 0, footer, ColorRole::Dim);
        }  // else 表单层

        scr->flush();
    };

    rebuild_avail();
    refresh_suggestions();

    // ==================== 交互循环 ====================
    while (!quit) {
        do_render();
        char32_t key = term->platform()->read_char();
        status_msg.clear();

        // 终端尺寸变更：清屏强制全量重绘（Screen::resize 保留 m_previous，
        // 差分会跳过内容未变行，故清物理屏 + 重置双缓冲）
        if (key == KEY_RESIZE) {
            term_width = term->get_terminal_width();
            term_height = term->get_terminal_height();
            overlay_bottom = term_height - 1;
            if (overlay_bottom < 1) overlay_bottom = 1;
            scr->resize(term_width, term_height);
            scr->clear_terminal();
            continue;
        }

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
                    // 注意：保留 dirty（列表层 Ctrl+D 等未保存修改的标记不因进入表单丢失），
                    // 表单层 Esc 放弃时统一清空
                    st.active = avail_indices[list_cursor];
                    st.field = F_NAME;
                    st.context_auto = true;
                    esc_armed = false;
                    editing_new = false;
                    entry_snapshot = st.providers[st.active];  // Esc 放弃时回滚编辑
                    refresh_suggestions();
                    layer = Layer::Form;
                    status_msg = std::format("编辑配置：{}", st.providers[st.active].name);
                } else {
                    status_msg = "暂无可用配置，按 Ctrl+N 新增";
                }
            } else if (key == KEY_TAB || key == ' ') {
                if (!avail_indices.empty()) {
                    // 设为使用中（绿色边框项）：写标量键 + 返回结果
                    result.entry = st.providers[avail_indices[list_cursor]];
                    apply_provider_switch(cfg, result.entry);
                    persist_list();  // 列表同步落盘（含首启迁移场景）
                    result.applied = true;
                    quit = true;
                } else {
                    status_msg = "暂无可用配置，按 Ctrl+N 新增";
                }
            } else if (key == KEY_CTRL_N) {
                // 新增配置 → 进入表单层
                // 生成唯一占位 id：循环递增直到不冲突（删除后再新增不会重复）
                std::string new_id;
                for (int n = 1;; ++n) {
                    std::string candidate = "new-provider-" + std::to_string(n);
                    bool taken = false;
                    for (const auto& e : st.providers) {
                        if (e.id == candidate) { taken = true; break; }
                    }
                    if (!taken) { new_id = std::move(candidate); break; }
                }
                ProviderConfigEntry ne;
                ne.id = std::move(new_id);
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
                    st.dirty = true;
                    rebuild_avail();
                    status_msg = "已删除配置";
                } else {
                    status_msg = "当前无可删除的可用配置";
                }
                esc_armed = false;
            } else if (key == KEY_CTRL_S) {
                persist_list();
                status_msg = "已保存配置列表";
            } else if (key == KEY_ESC || key == KEY_CTRL_C) {
                if (st.dirty && !esc_armed) {
                    esc_armed = true;
                    status_msg = "有未保存修改，再按一次 Esc 放弃（不保存）";
                    continue;
                }
                // 放弃所有未保存修改直接退出（想保存请按 Ctrl+S 或 Tab/空格 设为使用中）
                quit = true;
            }
            continue;
        }

        // ============ 第二层：表单 ============
        ProviderConfigEntry& cur = st.providers[st.active];
        bool suggest_visible = (st.field == F_NAME && !filtered_suggestions.empty());

        if (key == KEY_ENTER) {
            if (suggest_visible) {
                // 提示可见：填入选中项
                const auto& item = filtered_suggestions[suggest_selected];
                if (item.is_custom) {
                    cur.name = "Custom URL";
                    cur.id = CUSTOM_PROVIDER_ID;
                    cur.base_url.clear();
                    cur.model.clear();
                    cur.context_length = 0;
                    st.context_auto = true;
                    status_msg = "已选择自定义，请输入 Base URL";
                } else {
                    const ProviderPreset* p = find_preset(item.name);
                    if (p) {
                        cur.name = p->display_name;
                        cur.id = std::string(p->name);
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
                persist_list();
                rebuild_avail();
                layer = Layer::List;
                status_msg = editing_new
                    ? std::format("已添加供应商 {}", cur.name)
                    : std::format("已保存供应商 {}", cur.name);
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
                std::string id_copy = item.name;
                cur.name = display_copy;
                cur.id = id_copy;
                st.dirty = true;
                refresh_suggestions();
                status_msg = std::format("补全: {}", display_copy);
            }
            esc_armed = false;
        } else if (key == KEY_CTRL_S) {
            persist_list();
            rebuild_avail();
            layer = Layer::List;
            status_msg = "已保存配置列表";
        } else if (key == KEY_ESC || key == KEY_CTRL_C) {
            if (st.dirty && !esc_armed) {
                esc_armed = true;
                status_msg = "有未保存修改，再按一次 Esc 返回列表（不保存）";
                continue;
            }
            // 放弃（不保存）：撤销未完成的新增条目；编辑已有条目时回滚到进入表单前的快照
            if (editing_new) {
                st.providers.erase(st.providers.begin() + st.active);
                if (st.active >= static_cast<int>(st.providers.size())) {
                    st.active = static_cast<int>(st.providers.size()) - 1;
                }
                editing_new = false;
            } else {
                st.providers[st.active] = entry_snapshot;
            }
            st.dirty = false;  // 放弃所有未保存修改（含列表层的删除/新增标记）
            rebuild_avail();
            layer = Layer::List;
            esc_armed = false;
            status_msg = "已放弃修改，返回配置列表";
        } else if (key == KEY_BACKSPACE || key == KEY_DELETE) {
            if (st.field == F_CONTEXT) {
                cur.context_length /= 10;
                if (cur.context_length == 0) st.context_auto = true;
            } else {
                std::string* vp = field_value(cur, st.field);
                if (!vp || vp->empty()) continue;
                pop_utf8_back(*vp);
                st.dirty = true;
                esc_armed = false;
                if (st.field == F_MODEL) {
                    cur.context_length = match_context_window(catalog.get(), cur.model);
                    st.context_auto = (cur.context_length > 0);
                } else if (st.field == F_NAME) {
                    refresh_suggestions();
                }
            }
        } else if (key >= '0' && key <= '9' && st.field == F_CONTEXT) {
            // 上限与 backend.context_length schema 一致（2000000），防止 int32 溢出
            constexpr int64_t kMaxContext = 2000000;
            int64_t next = static_cast<int64_t>(cur.context_length) * 10 + (key - '0');
            if (next > kMaxContext) {
                status_msg = "上下文窗口已达上限 2000000";
            } else {
                if (cur.context_length == 0) st.context_auto = false;
                cur.context_length = static_cast<int32_t>(next);
                st.context_auto = false;
                st.dirty = true;
                esc_armed = false;
            }
        } else if (key >= 0x20 && key < 0xE000) {  // 可打印字符（含中文），0xE000+ 为特殊键区
            std::string* vp = field_value(cur, st.field);
            if (!vp) {
                // F_CONTEXT 只接受数字（数字已在上面分支处理）
                status_msg = "上下文窗口仅支持输入数字";
            } else if (vp->size() < 512) {
                append_utf8_cp(key, *vp);
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

    term->end_overlay();
    scr->reset_buffers();
    return result;
}

} // namespace agent
