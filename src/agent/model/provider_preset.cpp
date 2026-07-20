/**
 * @file provider_preset.cpp
 * @brief Provider 预设注册表实现
 */

#include "agent/model/provider_preset.h"
#include <algorithm>

namespace agent {

/// @brief 内置预设表
/// @note openai-compatible 是特殊预设：无默认 URL/Model，用户必须提供 --remote
/// @note F.9：默认模型版本会随厂商发布过时，定期更新并标注日期。
///       用户可通过 config.json 的 backend.model_name 覆盖默认值。
static const ProviderPreset s_presets[] = {
    {
        .name          = "openai",
        .display_name  = "OpenAI",
        .type          = ProviderType::OpenAI,
        .default_url   = "https://api.openai.com",
        .default_model = "gpt-4o-2024-11-20",  // F.9: 2025-01 更新，原 gpt-4o
        .api_path      = "/v1/chat/completions"
    },
    {
        .name          = "anthropic",
        .display_name  = "Anthropic",
        .type          = ProviderType::Anthropic,
        .default_url   = "https://api.anthropic.com",
        .default_model = "claude-sonnet-4-5-20250929",  // F.9: 2025-09 更新，原 claude-sonnet-4-20250514
        .api_path      = "/v1/messages"
    },
    {
        .name          = "deepseek",
        .display_name  = "DeepSeek",
        .type          = ProviderType::OpenAI,
        .default_url   = "https://api.deepseek.com",
        .default_model = "deepseek-chat",  // DeepSeek 自动指向最新版本，无需日期后缀
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
    // 注意：lambda 必须显式返回 string_view，而非 std::string
    // 若返回 std::string（值拷贝临时对象），string_view 会指向临时对象的内部 buffer，
    // 临时对象在 push_back 后析构，string_view 悬空 → 读取到无效内容
    std::transform(std::begin(s_presets), std::end(s_presets), std::back_inserter(names),
        [](const auto& preset) -> std::string_view { return preset.name; });
    return names;
}

std::optional<std::string> build_preset_url(const ProviderPreset* preset) {
    if (!preset || preset->default_url.empty()) {
        return std::nullopt;
    }

    std::string url = preset->default_url;
    // 去重尾部 /
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }

    if (!preset->api_path.empty()) {
        url += preset->api_path;
    }

    return url;
}

} // namespace agent
