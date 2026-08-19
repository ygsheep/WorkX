#include "widgets/sidebar.h"

#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

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

/// @brief 区块标题：米白
Element section_title(const std::string_view& title) {
    return ftxui::text(std::string(title)) | ftxui::color(kInfoText);
}

/// @brief 侧栏列表区块（MCP / TODO）：标题 + 条目列表；空则显示占位符
/// @note agent 侧 MCP / TODO 能力尚未实现，数据接口预留（items 当前恒为空）
void append_list_section(Elements& rows, const std::string_view& title,
                         const std::vector<std::string>& items) {
    rows.push_back(section_title(title));
    if (items.empty()) {
        rows.push_back(ftxui::hbox({
            ftxui::text("  "),
            ftxui::text(std::string(str::kDash)) | ftxui::color(theme::T::TextFaint),
        }));
        return;
    }
    for (const auto& it : items) {
        rows.push_back(ftxui::hbox({
            ftxui::text("  "),
            ftxui::flex(ftxui::text(it) | ftxui::color(theme::T::TextDim)),
        }));
    }
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

    // 基础信息：项目 / 分支 / Agent / 模型（项目、Agent、模型用米白）
    rows.push_back(kv(std::string(str::kSidebarProject),
                      s.project.empty() ? std::string(str::kDash) : s.project, kInfoText));
    if (!s.branch.empty())
        rows.push_back(kv(std::string(str::kSidebarBranch), s.branch));
    if (!s.agent.empty())
        rows.push_back(kv(std::string(str::kSidebarAgent), s.agent, kInfoText));
    rows.push_back(kv(std::string(str::kSidebarModel),
                      s.model.empty() ? std::string(str::kDash) : s.model, kInfoText));

    rows.push_back(sidebar_rule());

    // ── 3.1 统计：上下文 / DS 缓存 / Token / 成本 ──
    rows.push_back(section_title(str::kSidebarContext));
    rows.push_back(ftxui::hbox({
        build_context_gauge(s.context_used, s.context_limit),
    }));
    std::string ctx_str = std::to_string(s.context_used) + std::string(str::kSlash) +
                          (s.context_limit > 0 ? std::to_string(s.context_limit)
                                               : std::string(str::kInfinity));
    rows.push_back(ftxui::hbox({ftxui::text(ctx_str)}));

    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", s.cache_read_tokens);
        rows.push_back(kv(std::string(str::kSidebarCache), buf, kInfoText));
    }
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", s.total_tokens);
        rows.push_back(kv(std::string(str::kSidebarToken), buf, kInfoText));
    }
    {
        // 成本：agent 侧暂无成本链路，cost_usd 恒为 0，显示占位符
        std::string cost_str;
        if (s.cost_usd > 0.0) {
            char buf[64];
            snprintf(buf, sizeof(buf), "$%.4f", s.cost_usd);
            cost_str = buf;
        } else {
            cost_str = std::string(str::kDash);
        }
        rows.push_back(kv(std::string(str::kSidebarCost), cost_str, kInfoText));
    }

    rows.push_back(sidebar_rule());

    // ── 3.2 MCP 列表（agent 未实现，占位）──
    append_list_section(rows, str::kSidebarMCP, s.mcp_servers);

    rows.push_back(sidebar_rule());

    // ── 3.3 TODO 列表（agent 未实现，占位）──
    append_list_section(rows, str::kSidebarTODO, s.todos);

    return ftxui::vbox({
        ftxui::text(" "),   // 顶部内边距（1 行空白，实际占位）
        ftxui::hbox({
            ftxui::text("  "), // 左侧内边距（2 格）
            ftxui::flex(ftxui::vbox(std::move(rows))),
        }),
        ftxui::text(" "),   // 底部内边距（1 行空白，实际占位）
    });
}

}  // namespace ftxtui