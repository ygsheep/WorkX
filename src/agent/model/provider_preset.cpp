/**
 * @file provider_preset.cpp
 * @brief Provider 预设注册表实现
 */

#include "agent/model/provider_preset.h"
#include <algorithm>

namespace agent {

/// @brief 内置预设表
/// @note openai-compatible 是特殊预设：无默认 URL/Model，用户必须提供 --remote
static const ProviderPreset s_presets[] = {
    {
        .name          = "openai",
        .display_name  = "OpenAI",
        .type          = ProviderType::OpenAI,
        .default_url   = "https://api.openai.com",
        .default_model = "gpt-4o",
        .api_path      = "/v1/chat/completions"
    },
    {
        .name          = "anthropic",
        .display_name  = "Anthropic",
        .type          = ProviderType::Anthropic,
        .default_url   = "https://api.anthropic.com",
        .default_model = "claude-sonnet-4-20250514",
        .api_path      = "/v1/messages"
    },
    {
        .name          = "deepseek",
        .display_name  = "DeepSeek",
        .type          = ProviderType::OpenAI,
        .default_url   = "https://api.deepseek.com",
        .default_model = "deepseek-chat",
        .api_path      = "/v1/chat/completions"
    },
    {
        .name          = "groq",
        .display_name  = "Groq",
        .type          = ProviderType::OpenAI,
        .default_url   = "https://api.groq.com/openai",
        .default_model = "llama-3.3-70b-versatile",
        .api_path      = "/v1/chat/completions"
    },
    {
        .name          = "together",
        .display_name  = "Together AI",
        .type          = ProviderType::OpenAI,
        .default_url   = "https://api.together.xyz",
        .default_model = "mistralai/Mixtral-8x22B-Instruct-v0.1",
        .api_path      = "/v1/chat/completions"
    },
    {
        .name          = "lm-studio",
        .display_name  = "LM Studio",
        .type          = ProviderType::OpenAI,
        .default_url   = "http://localhost:1234",
        .default_model = "",
        .api_path      = "/v1/chat/completions",
        .timeout_ms    = 120000,
        .retry_delay_ms = 5000
    },
    {
        .name          = "openai-compatible",
        .display_name  = "Custom URL",
        .type          = ProviderType::OpenAI,
        .default_url   = "",
        .default_model = "",
        .api_path      = "/v1/chat/completions"
    }
};

const ProviderPreset* find_preset(std::string_view name) {
    auto it = std::find_if(std::begin(s_presets), std::end(s_presets),
        [&](const auto& preset) { return preset.name == name; });
    return it != std::end(s_presets) ? &*it : nullptr;
}

std::vector<std::string_view> list_preset_names() {
    std::vector<std::string_view> names;
    names.reserve(std::size(s_presets));
    std::transform(std::begin(s_presets), std::end(s_presets), std::back_inserter(names),
        [](const auto& preset) { return preset.name; });
    return names;
}

std::string build_preset_url(const ProviderPreset* preset) {
    if (!preset || preset->default_url.empty()) {
        return "(custom URL required)";
    }

    std::string url(preset->default_url);
    // 去重尾部 /
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }

    if (!preset->api_path.empty()) {
        url += std::string(preset->api_path);
    }

    return url;
}

} // namespace agent
