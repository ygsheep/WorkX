/**
 * @file provider_form.h
 * @brief 交互式供应商管理（/provider 命令 UI，两层 TUI）
 * @details 第一层：可用配置列表（绿色边框=使用中，白色边框=选中）；
 *          第二层：字段表单（Enter 依次切换，最后一个字段 Enter=保存）。
 *          多供应商列表通过 ConfigManager 持久化（backend.providers JSON 数组），
 *          设为使用中时写入 backend.* 标量键并返回结果供调用方热切换会话。
 *          领域类型与配置读写（load/save/apply_provider_switch）定义在
 *          agent/model/provider_config.h，本文件仅含 UI 层函数。
 * @version 2.1.0
 * @date 2026-08
 */

#pragma once

#include <string>
#include <vector>

#include "agent/model/provider_config.h"

namespace tui { class Terminal; class Screen; }

namespace agent {

class IConfigManager;

/// @brief 交互式供应商管理（/provider 命令）
/// @param cfg 配置管理器（加载/保存多供应商列表与使用中状态）
/// @param term 终端
/// @param scr 屏幕
/// @return 发生"设为使用中"时 applied=true；用户取消或仅编辑列表时 applied=false
ProviderSwitchResult provider_manager_interactive(
    IConfigManager& cfg,
    tui::Terminal* term, tui::Screen* scr);

} // namespace agent
