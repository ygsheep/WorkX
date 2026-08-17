#include "widgets/status_line.h"

#include <string>
#include <string_view>

#include "theme/theme.h"

namespace ftxtui {

using ftxui::Color;
using ftxui::Element;
using ftxui::Elements;

namespace {
/// @brief 权限模式展示规格（统一映射表）
/// icon：Nerd Font 私有区字符（需终端字体支持）；label：中文标签；color：主题色
struct PermSpec {
    std::string_view icon;
    std::string_view label;
    Color color;
};

/// @brief 权限模式 → 展示规格 映射表（key："" / "plan" / "bypass"）
/// @note 不用 constexpr：Color::xxx 非编译期常量
const PermSpec kPermTable[] = {
    {"\uF044", "计划模式", Color::CyanLight},    // 计划模式 · nf-fa-clipboard
    {"\uEB53", "完全访问", Color::YellowLight},  // 完全访问 · nf-cod-key
    {"\uF256", "手动审批", Color::GrayLight},    // 手动审批 · nf-fa-hand-paper
};
}  // namespace

Element build_status_line(const std::string& model,
                          const std::string& permission,
                          bool busy) {
    const PermSpec spec =
        (permission == "plan")   ? kPermTable[0]
        : (permission == "bypass") ? kPermTable[1]
                                   : kPermTable[2];

    // 左侧：权限模式（Nerd Font 图标 + 中文），替代原来的“就绪”状态
    std::string perm_text = std::string(spec.icon) + " " + std::string(spec.label);

    Elements seg;
    if (busy) {
        seg.push_back(ftxui::text("● 生成中") | ftxui::color(Color::YellowLight));
        seg.push_back(ftxui::text("  "));
    }
    seg.push_back(ftxui::color(spec.color)(ftxui::text(perm_text)));

    if (!model.empty()) {
        seg.push_back(ftxui::text("  "));
        seg.push_back(ftxui::text(model) | ftxui::color(theme::T::Text));
    }

    return ftxui::hbox(std::move(seg));
}

}  // namespace ftxtui