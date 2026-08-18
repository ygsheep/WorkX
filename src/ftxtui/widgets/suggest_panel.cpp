/**
 * @file suggest_panel.cpp
 * @brief 输入栏提示面板渲染 + 纯逻辑（parse_suggest_query / filter_commands）
 */

#include "widgets/suggest_panel.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>

#include "theme/strings.h"
#include "theme/theme.h"

namespace ftxtui {

using ftxui::Color;
using ftxui::Element;
using ftxui::Elements;

namespace {

/// @brief 小写化（过滤用）
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// @brief 面板边框色（弱提示灰）
const Color kBorderColor = theme::T::TextFaint;

}  // namespace

SuggestMode parse_suggest_query(const std::string& line, std::string& query) {
    query.clear();
    if (line.empty()) return SuggestMode::None;
    // 命令模式：行首 "/"
    if (line[0] == '/') {
        query = line.substr(1);
        return SuggestMode::Command;
    }
    // 文件模式：行内最后一个 "@"，其后无空格（对齐 src/tui bottom_bar_manager）
    const auto at = line.rfind('@');
    if (at != std::string::npos) {
        const std::string after = line.substr(at + 1);
        if (after.find(' ') == std::string::npos) {
            query = after;
            return SuggestMode::File;
        }
    }
    return SuggestMode::None;
}

std::vector<size_t> filter_commands(const std::vector<std::string>& commands,
                                    const std::string& query) {
    std::vector<size_t> hits;
    const std::string q = lower(query);
    for (size_t i = 0; i < commands.size(); ++i) {
        if (q.empty() || lower(commands[i]).find(q) != std::string::npos)
            hits.push_back(i);
    }
    return hits;
}

Element render_suggest_panel(SuggestMode mode,
                             const std::vector<SuggestEntry>& entries,
                             int selected,
                             bool file_ready) {
    using namespace ftxui;

    if (mode == SuggestMode::None) return emptyElement();

    // 空态文案（区分命令/文件；文件另有「索引构建中」）
    Element body;
    if (entries.empty()) {
        if (mode == SuggestMode::File && !file_ready) {
            body = text(std::string(str::kSuggestIndexing)) | color(theme::T::TextDim);
        } else {
            body = text(mode == SuggestMode::Command ? std::string(str::kSuggestNoCommand)
                                                     : std::string(str::kSuggestNoFile))
                   | color(theme::T::TextDim);
        }
    } else {
        Elements rows;
        for (size_t i = 0; i < entries.size(); ++i) {
            const auto& e = entries[i];
            const bool sel = (static_cast<int>(i) == selected);
            auto row = hbox({
                text(sel ? "  ❯ " : "    "),
                text(e.title) | color(theme::T::Text),
                e.subtitle.empty() ? flex(text("")) : flex(text("")),
                e.subtitle.empty()
                    ? emptyElement()
                    : text("  " + e.subtitle) | color(theme::T::TextFaint),
                text(" "),
            });
            if (sel) row = row | bgcolor(theme::T::Selection);
            rows.push_back(row);
        }
        body = vbox(std::move(rows));
    }

    // 高度上限 8 行；边框与输入区同宽（App 侧以 flex 填充）
    return vbox({
               text(" "),
               hbox({text("  "), body | flex, text("  ")}),
               text(" "),
           })
           | size(HEIGHT, LESS_THAN, 10)
           | bgcolor(theme::T::Panel)
           | border | color(kBorderColor);
}

}  // namespace ftxtui