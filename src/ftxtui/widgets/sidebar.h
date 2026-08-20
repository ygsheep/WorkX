/**
 * @file sidebar.h
 * @brief 右侧上下文/成本面板（opencode 双栏侧栏，窄屏自动折叠）
 */

#pragma once

#include <deque>

#include <ftxui/dom/elements.hpp>

#include "vm/view_model.h"

namespace ftxtui {

/// @brief 侧栏可折叠区块命中区（MCP / TODO 标题行，点击切换展开）
struct SectionHit {
    enum class Kind { kMCP, kTODO };
    Kind kind = Kind::kMCP;
    ftxui::Box box;
};

/// @brief 追加侧栏聚合信息（新会话 / 项目 / 分支 / 模型 / 上下文 / Token / 缓存 / MCP / TODO）
/// @param rows 目标行容器（追加到末尾）
/// @param section_hits 非空时记录可折叠区块标题命中区（鼠标点击用）
void append_sidebar_info(ftxui::Elements& rows, const SidebarModel& s,
                         std::deque<SectionHit>* section_hits = nullptr);

}  // namespace ftxtui
