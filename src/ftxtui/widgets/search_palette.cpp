/**
 * @file search_palette.cpp
 * @brief 全局聚合搜索面板实现（替代旧 command_palette）
 */

#include "widgets/search_palette.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include "theme/icons.h"
#include "theme/strings.h"
#include "theme/theme.h"

namespace ftxtui {

using ftxui::Color;
using ftxui::Element;
using ftxui::Elements;
using ftxui::Event;

namespace {

// 统一主题色（对应 theme/theme.h）
const Color kPanelBg = theme::T::Panel;      // 面板背景
const Color kAccent  = theme::T::Accent;     // 「⌕」/光标
const Color kSelBg   = theme::T::Selection;  // 选中项背景
constexpr int kMaxVisible = 10;              // 列表最多显示项数

/// @brief 类别标签（行右侧暗色显示）
std::string_view category_label(SearchCategory c) {
    switch (c) {
        case SearchCategory::Feature: return str::kCatFeature;
        case SearchCategory::File:    return str::kCatFile;
        case SearchCategory::Session: return str::kCatSession;
        case SearchCategory::Setting: return str::kCatSetting;
        case SearchCategory::Model:   return str::kCatModel;
        case SearchCategory::Provider:return str::kCatProvider;
    }
    return "";
}

/// @brief 小写化（过滤用）
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

}  // namespace

std::vector<int> filter_search_entries(const std::vector<SearchEntry>& entries,
                                       const std::string& query) {
    const std::string q = lower(query);
    std::vector<int> prefix_hits;
    std::vector<int> sub_hits;
    for (size_t i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        const std::string hay = lower(e.title + " " + e.subtitle + " " + e.keywords);
        if (q.empty() || hay.find(q) != std::string::npos) {
            // 标题前缀命中优先（排序规则 1）；其余保持数据源顺序（规则 2）
            if (!q.empty() && lower(e.title).find(q) == 0)
                prefix_hits.push_back(static_cast<int>(i));
            else
                sub_hits.push_back(static_cast<int>(i));
        }
    }
    prefix_hits.insert(prefix_hits.end(), sub_hits.begin(), sub_hits.end());
    return prefix_hits;
}

class SearchPalette : public ftxui::ComponentBase {
public:
    SearchPalette(std::vector<SearchEntry>& entries,
                  std::function<void(int)> on_select,
                  bool& open,
                  std::function<void()> on_close,
                  std::string title)
        : m_all(entries),
          m_on_select(std::move(on_select)),
          m_open(open),
          m_on_close(std::move(on_close)),
          m_title(std::move(title)) {
        refilter();
    }

    bool OnEvent(Event event) override {
        // Esc：先清空搜索，再关闭
        if (event == Event::Escape) {
            if (!m_search.empty()) { m_search.clear(); refilter(); return true; }
            close();
            return true;
        }
        // Enter：执行选中项（映射回原始下标）
        if (event == Event::Return) {
            if (m_selected >= 0 && static_cast<size_t>(m_selected) < m_order.size())
                if (m_on_select) m_on_select(m_order[static_cast<size_t>(m_selected)]);
            close();
            return true;
        }
        // 移动选择：↑↓ / Ctrl+N / Ctrl+P / Tab（Tab = 向下循环，等同 ArrowDown）
        if (event == Event::ArrowUp || event == Event::CtrlP) { move(-1); return true; }
        if (event == Event::ArrowDown || event == Event::CtrlN || event == Event::Tab) {
            move(+1);
            return true;
        }
        // 行编辑
        if (event == Event::CtrlU) { m_search.clear(); refilter(); return true; }
        if (event == Event::CtrlW) { delete_word(); return true; }
        if (event == Event::Backspace) {
            if (!m_search.empty()) { m_search.pop_back(); refilter(); }
            return true;
        }
        // 文本输入（UTF-8）
        if (event.is_character()) {
            m_search += event.character();
            refilter();
            return true;
        }
        return false;
    }

    Element OnRender() override {
        using namespace ftxui;

        // 打开边沿：清空搜索框；每次渲染重新过滤（entries 由调用方装配/更新，
        // 如会话列表后台加载完成）。选择位置在过滤后保留（clamp）。
        if (m_open && !m_last_open) m_search.clear();
        m_last_open = m_open;
        refilter();

        // 标题行（面板标题，如「恢复会话」）
        Elements head;
        if (!m_title.empty()) {
            head.push_back(hbox({
                text("  "),
                text(m_title) | color(theme::T::Accent) | bold,
                flex(text("")),
            }));
        }

        // 顶部搜索行
        Element input_field = hbox({
            text("⌕ ") | color(kAccent),
            m_search.empty()
                ? text(std::string(str::kPaletteSearchHint)) | color(theme::T::Text)
                : hbox({text(m_search), text("▎") | color(kAccent)}),
            flex(text("")),
        });

        // 列表（滚动窗口，保证选中项可见）
        Elements list;
        if (m_order.empty()) {
            list.push_back(text(std::string(str::kPaletteNoMatch)) | color(theme::T::Text));
        } else {
            const size_t start = m_start;
            const size_t end = std::min(m_start + static_cast<size_t>(kMaxVisible),
                                        m_order.size());
            for (size_t i = start; i < end; ++i) {
                const SearchEntry& se = m_all[static_cast<size_t>(m_order[i])];
                const bool sel = (static_cast<int>(i) == m_selected);
                // 使用中条目行首 ●（accent 色），选中行 ❯，其余空白
                std::string marker = se.active ? "  ● " : (sel ? "  ❯ " : "    ");
                auto marker_elem = text(marker)
                    | color(se.active ? theme::T::Accent : ftxui::Color::Default);
                auto row = hbox({
                    marker_elem,
                    text(se.title.empty() ? se.subtitle : se.title),
                    flex(text("")),
                    se.subtitle.empty()
                        ? emptyElement()
                        : (text(" " + se.subtitle) | color(theme::T::TextFaint)),
                    text("  ") | color(theme::T::TextFaint),
                    text(std::string(category_label(se.category)))
                        | color(theme::T::TextFaint),
                    text(" "),
                });
                if (sel) row = row | bgcolor(kSelBg) | color(theme::T::Text) | bold;
                list.push_back(row);
            }
            const size_t hidden = m_order.size() - end;
            if (hidden > 0)
                list.push_back(text(std::string(str::kPaletteMorePrefix) +
                                    std::to_string(hidden) +
                                    std::string(str::kPaletteMoreSuffix)) |
                               color(theme::T::Text));
        }

        Elements content_elems;
        for (auto& h : head) content_elems.push_back(std::move(h));
        content_elems.push_back(input_field);
        content_elems.push_back(separatorEmpty());
        content_elems.push_back(vbox(std::move(list)));
        content_elems.push_back(separatorEmpty());
        content_elems.push_back(text(std::string(str::kPaletteHint)) | color(theme::T::Text));
        auto content = vbox(std::move(content_elems));

        // 内容四周留白，面板悬浮于主会话之上
        return vbox({
                   text(" "),
                   hbox({ text("  "), content | flex, text("  ") }),
                   text(" "),
               })
               | size(WIDTH, LESS_THAN, 70)
               | size(HEIGHT, LESS_THAN, kMaxVisible + 6)
               | bgcolor(kPanelBg)
               | border;
    }

private:
    void refilter() {
        m_order = filter_search_entries(m_all, m_search);
        if (m_order.empty()) {
            m_selected = -1;
        } else {
            if (m_selected < 0) m_selected = 0;
            m_selected = std::min(m_selected, static_cast<int>(m_order.size()) - 1);
        }
        update_view();
    }

    void move(int delta) {
        if (m_order.empty()) return;
        const int n = static_cast<int>(m_order.size());
        m_selected = (m_selected + delta) % n;
        if (m_selected < 0) m_selected += n;
        update_view();
    }

    void delete_word() {
        size_t i = m_search.size();
        while (i > 0 && m_search[i - 1] == ' ') --i;
        while (i > 0 && m_search[i - 1] != ' ') --i;
        if (i == m_search.size()) return;  // 已到行首
        m_search.erase(i);
        refilter();
    }

    void close() {
        m_open = false;
        if (m_on_close) m_on_close();
    }

    void update_view() {
        const size_t n = m_order.size();
        if (m_selected < 0) { m_start = 0; return; }
        const size_t sel = static_cast<size_t>(m_selected);
        if (sel < m_start) m_start = sel;
        else if (sel >= m_start + static_cast<size_t>(kMaxVisible))
            m_start = sel - (static_cast<size_t>(kMaxVisible) - 1);
        const size_t max_start =
            n > static_cast<size_t>(kMaxVisible) ? n - kMaxVisible : 0;
        m_start = std::min(m_start, max_start);
    }

    std::vector<SearchEntry>& m_all;
    std::vector<int> m_order;              // 过滤后位置 → m_all 原始下标
    std::string m_search;
    std::string m_title;                   // 面板标题（搜索框上方）
    int m_selected = -1;
    size_t m_start = 0;
    bool m_last_open = false;              // 打开边沿检测（清空搜索框）
    std::function<void(int)> m_on_select;
    bool& m_open;
    std::function<void()> m_on_close;
};

ftxui::Component make_search_palette(std::vector<SearchEntry>& entries,
                                     std::function<void(int)> on_select,
                                     bool& open,
                                     std::function<void()> on_close,
                                     std::string title) {
    return ftxui::Make<SearchPalette>(entries, std::move(on_select),
                                      open, std::move(on_close),
                                      std::move(title));
}

}  // namespace ftxtui