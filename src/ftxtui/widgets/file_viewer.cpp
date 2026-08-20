#include "widgets/file_viewer.h"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/terminal.hpp>

#include "core/utils/line_diff.h"
#include "theme/strings.h"
#include "theme/theme.h"

namespace ftxtui {

using ftxui::Element;
using ftxui::Elements;

namespace {

/// @brief 行号列宽度（按最大行号位数）
int line_number_width(std::size_t line_count) {
    int digits = 1;
    std::size_t n = line_count;
    while (n >= 10) {
        n /= 10;
        ++digits;
    }
    return digits;
}

/// @brief 可视行数估算：侧栏内容区高度 − 路径栏/分隔线/状态栏
/// @details 侧栏整体占满终端高度：顶部内边距 1 + tab 栏 1 + 分隔线 1；
///          内容区内文件查看器再占 路径栏 1 + 分隔线 1 + 状态栏 1 + 分隔线 1。
int visible_line_count() {
    const int term_h = ftxui::Terminal::Size().dimy;
    return std::max(1, term_h - 7);
}

}  // namespace

Element build_file_viewer(const FileViewState& file) {
    if (file.path.empty()) {
        return ftxui::vbox({
            ftxui::text(" "),
            ftxui::hbox({
                ftxui::text("  "),
                ftxui::text(std::string(str::kTabFilesEmpty))
                    | ftxui::color(theme::T::TextFaint),
            }),
        });
    }

    // 路径栏：路径 + 行数 + 语言
    std::string meta = std::to_string(file.lines.size()) + std::string(str::kViewLineSuffix);
    if (!file.lang.empty()) meta += std::string(str::kViewLangSep) + file.lang;

    // 虚拟化滚动：可视行切片（行高固定 1）
    const int visible = visible_line_count();
    const int num_w = line_number_width(file.lines.size());
    const int scroll = std::max(0, file.scroll);

    // 内联 diff 高亮映射：绝对行号（1-based）→ 变更类型（Insert/Modify）
    std::map<int, agent::DiffKind> diff_mark;
    for (const auto& ch : file.changes) {
        if (ch.new_start <= 0) continue;
        for (const auto& dl : ch.diff) {
            if (dl.kind == agent::DiffKind::Equal) continue;
            diff_mark[ch.new_start + dl.line_no - 1] = dl.kind;
        }
    }

    Elements line_els;
    line_els.reserve(static_cast<std::size_t>(visible));
    for (int i = 0; i < visible; ++i) {
        const int line_idx = scroll + i;
        if (line_idx >= static_cast<int>(file.lines.size())) {
            line_els.push_back(ftxui::text(""));
            continue;
        }
        std::string num = std::to_string(line_idx + 1);
        while (static_cast<int>(num.size()) < num_w) num = " " + num;
        const bool changed = diff_mark.count(line_idx + 1) != 0;
        line_els.push_back(ftxui::hbox({
            ftxui::text(changed ? "+" : " ") | ftxui::color(theme::T::DiffAdd),
            ftxui::text(num) | ftxui::color(changed ? theme::T::DiffAdd : theme::T::TextFaint),
            ftxui::text(" "),
            ftxui::text(file.lines[static_cast<std::size_t>(line_idx)])
                | ftxui::color(changed ? theme::T::DiffAdd : theme::T::TextDim),
        }));
    }

    return ftxui::vbox({
        ftxui::hbox({
            ftxui::text(" "),
            ftxui::text(file.path) | ftxui::color(theme::T::Text),
            ftxui::flex(ftxui::text("")),
            ftxui::text(meta) | ftxui::color(theme::T::TextFaint),
            ftxui::text(" "),
        }),
        ftxui::separator() | ftxui::color(theme::T::TextFaint),
        ftxui::vbox(std::move(line_els)) | ftxui::yflex,
        ftxui::separator() | ftxui::color(theme::T::TextFaint),
        ftxui::hbox({
            ftxui::text("  "),
            ftxui::text(std::string(str::kViewScrollHint))
                | ftxui::color(theme::T::TextFaint),
        }),
    });
}

}  // namespace ftxtui
