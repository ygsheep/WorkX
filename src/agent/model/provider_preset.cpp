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
        .default_model = "gpt-5",  // 2025-08 发布，400K 上下文
        .api_path      = "/v1/chat/completions",
        .default_context_length = 400000  // GPT-5 系列
    },
    {
        .name          = "anthropic",
        .display_name  = "Anthropic",
        .type          = ProviderType::Anthropic,
        .default_url   = "https://api.anthropic.com",
        .default_model = "claude-opus-4-5-20251101",  // 2025-11-25 发布，旗舰编码模型
        .api_path      = "/v1/messages",
        .default_context_length = 200000  // Claude 4.5 系列（1M 需 beta 申请）
    },
    {
        .name          = "deepseek",
        .display_name  = "DeepSeek",
        .type          = ProviderType::OpenAI,
        .default_url   = "https://api.deepseek.com",
        .default_model = "deepseek-v4-flash",  // DeepSeek V4-Flash (2026-04-24) 非思考模式，1M context
        .api_path      = "/v1/chat/completions",
        .default_context_length = 1000000  // DeepSeek V4 系列 1M
    },
    {
        .name          = "deepseek-anthropic",
        .display_name  = "DeepSeek (Anthropic 兼容)",
        .type          = ProviderType::Anthropic,
        .default_url   = "https://api.deepseek.com/anthropic",
        .default_model = "deepseek-v4-flash",
        .api_path      = "/v1/messages",
        .default_context_length = 128000  // DeepSeek Anthropic 兼容端点
    },
    {
        .name          = "groq",
        .display_name  = "Groq",
        .type          = ProviderType::OpenAI,
        .default_url   = "https://api.groq.com/openai",
        .default_model = "llama-4-maverick-17b-128e-instruct",  // Llama 4 Maverick，1M 上下文
        .api_path      = "/v1/chat/completions",
        .default_context_length = 1000000  // Llama 4 Maverick 1M
    },
    {
        .name          = "together",
        .display_name  = "Together AI",
        .type          = ProviderType::OpenAI,
        .default_url   = "https://api.together.xyz",
        .default_model = "meta-llama/Llama-4-Maverick-17B-128E-Instruct-FP8",
        .api_path      = "/v1/chat/completions",
        .default_context_length = 1000000  // Llama 4 Maverick 1M
    },
    {
        .name          = "glm",
        .display_name  = "智谱 GLM (Z.ai)",
        .type          = ProviderType::OpenAI,
        .default_url   = "https://open.bigmodel.cn/api/paas/v4",
        .default_model = "glm-5.2",  // GLM-5.2 (2026-06-13) 旗舰编码模型，1M context
        .api_path      = "/chat/completions",
        .default_context_length = 1000000  // GLM-5.2 系列 1M
    },
    {
        .name          = "lm-studio",
        .display_name  = "LM Studio",
        .type          = ProviderType::OpenAI,
        .default_url   = "http://localhost:1234",
        .default_model = "",
        .api_path      = "/v1/chat/completions",
        .timeout_ms    = 120000,
        .retry_delay_ms = 10000   // 本地模型重试延迟 10s（指数退避上限 60s）
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
