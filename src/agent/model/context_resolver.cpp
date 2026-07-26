/**
 * @file context_resolver.cpp
 * @brief 上下文窗口解析器实现
 */

#include "agent/model/context_resolver.h"
#include "agent/model/config.h"
#include "agent/model/provider_preset.h"

namespace agent {

ContextLengthResolution resolve_context_length(
    std::string_view model_name,
    int32_t sel_context_length,
    int32_t cfg_context_length,
    const ProviderPreset* preset)
{
    ContextLengthResolution result;

    // 1. Provider list_models() 返回值（动态，最准确）
    if (sel_context_length > 0) {
        result.value = sel_context_length;
        result.source = ContextLengthResolution::Source::ProviderList;
        return result;
    }

    // 2. cfg.backend.context_length（用户历史/手动配置）
    if (cfg_context_length > 0) {
        result.value = cfg_context_length;
        result.source = ContextLengthResolution::Source::UserConfig;
        return result;
    }

    // 3. find_model_capability(model_name)（静态能力表）
    if (auto* cap = find_model_capability(model_name)) {
        if (cap->context_window > 0) {
            result.value = cap->context_window;
            result.source = ContextLengthResolution::Source::ModelCapability;
            return result;
        }
    }

    // 4. preset->default_context_length（per-provider 兜底）
    if (preset && preset->default_context_length > 0) {
        result.value = preset->default_context_length;
        result.source = ContextLengthResolution::Source::PresetDefault;
        return result;
    }

    // 5. MODEL_CONTEXT_WINDOW_DEFAULT（常量兜底）
    result.value = MODEL_CONTEXT_WINDOW_DEFAULT;
    result.source = ContextLengthResolution::Source::Default;
    return result;
}

} // namespace agent
