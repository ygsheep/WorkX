#include "widgets/sidebar.h"

#include <cstdio>
#include <string>

#include <ftxui/dom/direction.hpp>

#include "render/markdown_to_elements.h"
#include "theme/strings.h"
#include "theme/theme.h"

namespace ftxtui {

using ftxui::Color;
using ftxui::Element;
using ftxui::Elements;

namespace {
/// 信息行文字：米白色（统一主题）
const Color kInfoText = theme::T::Text;

/// @brief 键值行；默认键灰暗、值默认色；传入 text 时整行（键+值）用该颜色
Element kv(const std::string& key, const std::string& value,
           Color text = Color::Default) {
    auto key_el = ftxui::text(key);
    auto val_el = ftxui::text(value);
    if (text == Color::Default) {
        key_el |= ftxui::color(theme::T::Text);
        val_el |= ftxui::color(theme::T::Text);
    } else {
        key_el |= ftxui::color(text);
        val_el |= ftxui::color(text);
    }
    return ftxui::hbox({
        key_el,
        ftxui::separatorEmpty(),
        ftxui::flex(val_el),
    });
}
}  // namespace

Element sidebar_rule() {
    return ftxui::emptyElement();
}

Element build_sidebar(const SidebarModel& s) {
    Elements rows;

    // 标题
    std::string title = s.title.empty() ? std::string(str::kSidebarNewSession) : s.title;
    rows.push_back(ftxui::text(title) | ftxui::bold);
    rows.push_back(sidebar_rule());

    // 项目 / 分支 / Agent（项目与 Agent 文本用米白色）
    rows.push_back(kv(std::string(str::kSidebarProject),
                      s.project.empty() ? std::string(str::kDash) : s.project, kInfoText));
    if (!s.branch.empty())
        rows.push_back(kv(std::string(str::kSidebarBranch), s.branch));
    if (!s.agent.empty())
        rows.push_back(kv(std::string(str::kSidebarAgent), s.agent, kInfoText));

    rows.push_back(sidebar_rule());

    // 上下文占用
    rows.push_back(ftxui::text(std::string(str::kSidebarContext)));
    rows.push_back(ftxui::hbox({
        build_context_gauge(s.context_used, s.context_limit),
    }));
    std::string ctx_str = std::to_string(s.context_used) + std::string(str::kSlash) +
                          (s.context_limit > 0 ? std::to_string(s.context_limit)
                                               : std::string(str::kInfinity));
    rows.push_back(ftxui::hbox({ftxui::text(ctx_str)}));

    // 成本
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "$%.4f", s.cost_usd);
        rows.push_back(kv(std::string(str::kSidebarCost), buf, kInfoText));
    }

    // 模型
    rows.push_back(kv(std::string(str::kSidebarModel),
                      s.model.empty() ? std::string(str::kDash) : s.model, kInfoText));

    return ftxui::vbox({
        ftxui::emptyElement(),   // 顶部内边距
        ftxui::emptyElement(),
        ftxui::hbox({
            ftxui::text("  "),   // 左侧内边距
            ftxui::flex(ftxui::vbox(std::move(rows))),
        }),
        ftxui::emptyElement(),   // 底部内边距
        ftxui::emptyElement(),
    });
}

}  // namespace ftxtui