#include "widgets/command_palette.h"

#include <algorithm>
#include <cctype>
#include <utility>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

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
const Color kSelText = theme::T::Text;       // 选中项文字：米白色
constexpr int kMaxVisible = 10;                              // 列表最多显示项数

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

class CommandPalette : public ftxui::ComponentBase {
public:
    CommandPalette(std::vector<PaletteCommand> commands,
                   std::function<void(int)> on_select,
                   bool& open,
                   std::function<void()> on_close)
        : m_all(std::move(commands)),
          m_on_select(std::move(on_select)),
          m_open(open),
          m_on_close(std::move(on_close)) {
        // 预拼搜索文本：命令 + 标题 + 关键词（小写，供引擎式匹配）
        m_search_text.reserve(m_all.size());
        for (const auto& c : m_all) {
            m_search_text.push_back(
                lower(c.command + " " + c.title + " " + c.keywords));
        }
        refilter();
    }

    bool OnEvent(Event event) override {
        // Esc：先清空搜索，再关闭
        if (event == Event::Escape) {
            if (!m_search.empty()) { m_search.clear(); refilter(); return true; }
            close();
            return true;
        }
        // Enter：运行选中项（映射回原始下标）
        if (event == Event::Return) {
            if (m_selected >= 0 && static_cast<size_t>(m_selected) < m_order.size())
                if (m_on_select) m_on_select(m_order[static_cast<size_t>(m_selected)]);
            close();
            return true;
        }
        // 移动选择
        if (event == Event::ArrowUp || event == Event::CtrlP) { move(-1); return true; }
        if (event == Event::ArrowDown || event == Event::CtrlN) { move(+1); return true; }
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

        // 顶部搜索行
        Element input_field = hbox({
            text("⌕ ") | color(kAccent),
            m_search.empty()
                ? text("搜索命令（支持中/英文）…") | color(theme::T::Text)
                : hbox({text(m_search), text("▎") | color(kAccent)}),
            flex(text("")),
        });

        // 列表（滚动窗口，保证选中项可见）
        Elements list;
        if (m_order.empty()) {
            list.push_back(text("  无匹配命令") | color(theme::T::Text));
        } else {
            const size_t start = m_start;
            const size_t end = std::min(m_start + static_cast<size_t>(kMaxVisible),
                                        m_order.size());
            for (size_t i = start; i < end; ++i) {
                const PaletteCommand& pc = m_all[static_cast<size_t>(m_order[i])];
                const bool sel = (static_cast<int>(i) == m_selected);
                auto cmd_text = text("  " + pc.command);
                auto row = hbox({
                    text(sel ? "  ❯ " : "    "),
                    text(pc.title.empty() ? pc.command : pc.title),
                    flex(text("")),
                    // 未选中时命令暗色；选中时恢复本色，统一使用 kSelText 米白
                    sel ? cmd_text : (cmd_text | color(theme::T::Text)),
                });
                if (sel) row = row | bgcolor(kSelBg) | color(kSelText) | bold;
                list.push_back(row);
            }
            const size_t hidden = m_order.size() - end;
            if (hidden > 0)
                list.push_back(text("  ··· 还有 " + std::to_string(hidden) + " 项") | color(theme::T::Text));
        }

        auto content = vbox({
            input_field,
            separatorEmpty(),
            vbox(std::move(list)),
            separatorEmpty(),
            text("↑↓ 选择 · Enter 运行 · Esc 清除/关闭") | color(theme::T::Text),
        });

        // 内容四周留白（上下边距 + 左右边距），面板悬浮于主会话之上、不遮挡背景
        return vbox({
                   text(" "),
                   hbox({ text("  "), content | flex, text("  ") }),
                   text(" "),
               })
               | size(WIDTH, LESS_THAN, 60)
               | size(HEIGHT, LESS_THAN, kMaxVisible + 6)
               | bgcolor(kPanelBg)
               | border;
    }

private:
    void refilter() {
        m_order.clear();
        const std::string q = lower(m_search);
        for (size_t i = 0; i < m_search_text.size(); ++i) {
            if (q.empty() || m_search_text[i].find(q) != std::string::npos)
                m_order.push_back(static_cast<int>(i));
        }
        m_selected = m_order.empty() ? -1 : 0;
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

    std::vector<PaletteCommand> m_all;
    std::vector<std::string> m_search_text;  // 每条的可搜索文本（小写）
    std::vector<int> m_order;                // 过滤后位置 → m_all 原始下标
    std::string m_search;
    int m_selected = -1;
    size_t m_start = 0;
    std::function<void(int)> m_on_select;
    bool& m_open;
    std::function<void()> m_on_close;
};

}  // namespace

ftxui::Component make_command_palette(std::vector<PaletteCommand> commands,
                                      std::function<void(int)> on_select,
                                      bool& open,
                                      std::function<void()> on_close) {
    return ftxui::Make<CommandPalette>(std::move(commands), std::move(on_select),
                                       open, std::move(on_close));
}

}  // namespace ftxtui