/**
 * @file provider_selector.h
 * @brief 交互式供应商选择（/provider 命令 UI）
 * @details 复用 SelectPanel 展示内置供应商预设（deepseek/glm/kimi/qwen/minimax/custom），
 *          选中后进入内联输入模式采集 API Key（每次必输），
 *          custom 预设额外采集 URL 与模型名。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <string>

namespace tui { class Terminal; class Screen; }

namespace agent {

class IConfigManager;

/// @brief 供应商选择结果
/// @details select_provider_interactive 返回，空 provider 表示取消
struct ProviderSelection {
    std::string provider;   ///< 预设内部名，如 "deepseek"；custom 时为 "openai-compatible"
    std::string remote_url; ///< 完整 API 基础 URL（预设默认 URL 或 custom 输入）
    std::string api_path;   ///< API 路径，如 "/v1/chat/completions"
    std::string model_name; ///< 预设默认模型或 custom 输入
    std::string api_key;    ///< 用户本次输入的 API Key
};

/// @brief 交互式供应商选择（/provider 命令）
/// @param cfg 配置管理器（读取当前 provider 作为初始光标）
/// @param term 终端
/// @param scr 屏幕
/// @return 选中的供应商配置；provider 为空表示取消
ProviderSelection select_provider_interactive(
    IConfigManager& cfg,
    tui::Terminal* term, tui::Screen* scr);

/// @brief 将供应商选择应用到配置（写入 PROVIDER/REMOTE_URL/API_KEY/MODEL_NAME，
///        并清除 CONTEXT_LENGTH 让 catalog/静态表重新解析新供应商模型窗口）
/// @param cfg 配置管理器
/// @param sel 供应商选择结果
void apply_provider_selection(IConfigManager& cfg, const ProviderSelection& sel);

} // namespace agent
