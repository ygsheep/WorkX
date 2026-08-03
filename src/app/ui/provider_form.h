/**
 * @file provider_form.h
 * @brief 交互式供应商管理（/provider 命令 UI，两层 TUI）
 * @details 第一层：可用配置列表（绿色边框=使用中，白色边框=选中）；
 *          第二层：字段表单（Enter 依次切换，最后一个字段 Enter=保存）。
 *          多供应商列表通过 ConfigManager 持久化（backend.providers JSON 数组），
 *          设为使用中时写入 backend.* 标量键并返回结果供调用方热切换会话。
 * @version 2.0.0
 * @date 2026-08
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tui { class Terminal; class Screen; }

namespace agent {

class IConfigManager;

/// @brief 供应商配置条目（与持久化 JSON 字段一一对应）
struct ProviderConfigEntry {
    std::string id;              ///< 标识（预设内部名或自定义名称，写入 backend.provider）
    std::string name;            ///< 显示名
    std::string base_url;        ///< API 基础 URL
    std::string model;           ///< 模型 ID
    int32_t context_length = 0;  ///< 上下文窗口（token），0 = 未知
    std::string api_key;         ///< API Key
};

/// @brief 供应商面板返回结果
struct ProviderSwitchResult {
    bool applied = false;        ///< 是否执行了"设为使用中"（需要热切换）
    ProviderConfigEntry entry;   ///< 切换到的条目（applied=true 时有效）
};

/// @brief 从 ConfigManager 加载供应商列表（backend.providers，空时返回空）
std::vector<ProviderConfigEntry> load_provider_configs(IConfigManager& cfg);

/// @brief 保存供应商列表到 ConfigManager（backend.providers）
void save_provider_configs(IConfigManager& cfg,
                           const std::vector<ProviderConfigEntry>& providers);

/// @brief 将条目应用为使用中（写入 PROVIDER/REMOTE_URL/MODEL_NAME/API_KEY/CONTEXT_LENGTH）
void apply_provider_switch(IConfigManager& cfg, const ProviderConfigEntry& entry);

/// @brief 交互式供应商管理（/provider 命令）
/// @param cfg 配置管理器（加载/保存多供应商列表与使用中状态）
/// @param term 终端
/// @param scr 屏幕
/// @return 发生"设为使用中"时 applied=true；用户取消或仅编辑列表时 applied=false
ProviderSwitchResult provider_manager_interactive(
    IConfigManager& cfg,
    tui::Terminal* term, tui::Screen* scr);

} // namespace agent
