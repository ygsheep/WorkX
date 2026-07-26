/**
 * @file context_resolver.h
 * @brief 上下文窗口解析器
 * @details 统一入口，按优先级解析模型上下文窗口大小。
 *          优先级（高到低）：
 *            1. Provider list_models() 返回值（动态，最准确）
 *            2. cfg.backend.context_length（用户历史/手动配置）
 *            3. find_model_capability(model_name)（静态能力表，对齐 claude-code）
 *            4. preset->default_context_length（per-provider 兜底）
 *            5. MODEL_CONTEXT_WINDOW_DEFAULT（常量兜底）
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <cstdint>
#include <string_view>

namespace agent {

struct ProviderPreset;

/// @brief 上下文窗口解析结果
struct ContextLengthResolution {
    int32_t value = 0;          ///< 解析出的上下文窗口（token），>0 表示有效
    enum class Source {
        Default,            ///< 最终常量兜底
        PresetDefault,      ///< preset 默认值
        ModelCapability,    ///< 静态能力表
        UserConfig,         ///< cfg.backend.context_length
        ProviderList,       ///< provider list_models 返回
    } source = Source::Default;
};

/// @brief 统一解析模型上下文窗口
/// @param model_name 当前模型名（用于查 capability 表）
/// @param sel_context_length selector 返回值（来自 provider list_models，0 表示未提供）
/// @param cfg_context_length cfg.backend.context_length 持久化值（0 表示未配置）
/// @param preset 当前 provider 预设（可空）
/// @return 解析结果；value>0 时有效，source 标识来源
ContextLengthResolution resolve_context_length(
    std::string_view model_name,
    int32_t sel_context_length,
    int32_t cfg_context_length,
    const ProviderPreset* preset);

} // namespace agent
