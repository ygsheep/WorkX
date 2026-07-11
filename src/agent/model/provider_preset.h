/**
 * @file provider_preset.h
 * @brief Provider 预设注册表
 * @details 内置提供商预设（OpenAI、Anthropic、DeepSeek 等），
 *          用户只需选择名称 + 输入 API Key
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <string_view>
#include <vector>
#include "agent/model/provider_type.h"

namespace workx {

/// @brief Provider 预设
/// @details 一个预设包含名称、协议类型、默认 URL 和默认模型
struct ProviderPreset {
    std::string_view name;          ///< 内部名称，如 "deepseek"
    std::string_view display_name;  ///< 显示名，如 "DeepSeek"
    ProviderType type;              ///< 协议类型
    std::string_view default_url;   ///< 默认 API 基础 URL
    std::string_view default_model; ///< 默认模型名
    std::string_view api_path;      ///< API 路径，如 "/v1/chat/completions"
    int timeout_ms = 0;             ///< 预设超时（毫秒），0 表示使用全局默认
    int retry_delay_ms = 0;         ///< 预设重试延迟（毫秒），0 表示使用全局默认
};

/// @brief 构建完整的 Preset URL（base_url + api_path，去重尾部 /）
/// @param preset Preset 指针（nullptr 时返回空字符串）
/// @return 完整 API URL，如 "https://api.deepseek.com/v1/chat/completions"
std::string build_preset_url(const ProviderPreset* preset);

/// @brief 查找预设
/// @param name 预设名称（小写，如 "openai", "anthropic", "deepseek"）
/// @return 找到返回指针，否则 nullptr
const ProviderPreset* find_preset(std::string_view name);

/// @brief 获取所有预设名列表
/// @return 预设名称的 vector（如 ["openai", "anthropic", "deepseek", ...]）
std::vector<std::string_view> list_preset_names();

} // namespace workx
