/**
 * @file config.cpp
 * @brief 模型静态配置表实现
 * @details 内置模型能力表，按厂商规范维护。
 *          厂商发布新模型或调整 token 限制时需手动更新此表。
 *          日期标注便于跟踪更新时间。
 */

#include "agent/model/config.h"
#include <algorithm>
#include <cctype>
#include <cstring>

namespace agent {

namespace {

/// @brief 内置模型能力表
/// @note 更新日期：2026-07-20
/// @note 字段来源：Anthropic/OpenAI/DeepSeek 官方文档
/// @note F.9：模型版本会随厂商发布过时，定期更新并标注日期
static const ModelCapability s_capabilities[] = {
    // ============================================================
    // Anthropic Claude 系列
    // ============================================================
    {.canonical_name = "claude-sonnet-4-5",
     .context_window = 200'000,
     .max_output_tokens = 16'384,
     .supports_tools = true,
     .supports_vision = true},
    {.canonical_name = "claude-sonnet-4",
     .context_window = 200'000,
     .max_output_tokens = 16'384,
     .supports_tools = true,
     .supports_vision = true},
    {.canonical_name = "claude-opus-4-1",
     .context_window = 200'000,
     .max_output_tokens = 32'000,
     .supports_tools = true,
     .supports_vision = true},
    {.canonical_name = "claude-opus-4",
     .context_window = 200'000,
     .max_output_tokens = 4'096,
     .supports_tools = true,
     .supports_vision = true},
    {.canonical_name = "claude-haiku-4-5",
     .context_window = 200'000,
     .max_output_tokens = 8'192,
     .supports_tools = true,
     .supports_vision = true},
    {.canonical_name = "claude-3-7-sonnet",
     .context_window = 200'000,
     .max_output_tokens = 8'192,
     .supports_tools = true,
     .supports_vision = true},
    {.canonical_name = "claude-3-5-sonnet",
     .context_window = 200'000,
     .max_output_tokens = 8'192,
     .supports_tools = true,
     .supports_vision = true},
    {.canonical_name = "claude-3-5-haiku",
     .context_window = 200'000,
     .max_output_tokens = 8'192,
     .supports_tools = true,
     .supports_vision = false},
    {.canonical_name = "claude-3-opus",
     .context_window = 200'000,
     .max_output_tokens = 4'096,
     .supports_tools = true,
     .supports_vision = true},
    {.canonical_name = "claude-3-sonnet",
     .context_window = 200'000,
     .max_output_tokens = 4'096,
     .supports_tools = true,
     .supports_vision = true},
    {.canonical_name = "claude-3-haiku",
     .context_window = 200'000,
     .max_output_tokens = 4'096,
     .supports_tools = true,
     .supports_vision = false},

    // ============================================================
    // OpenAI GPT 系列
    // ============================================================
    {.canonical_name = "gpt-4o",
     .context_window = 128'000,
     .max_output_tokens = 16'384,
     .supports_tools = true,
     .supports_vision = true},
    {.canonical_name = "gpt-4o-mini",
     .context_window = 128'000,
     .max_output_tokens = 16'384,
     .supports_tools = true,
     .supports_vision = true},
    {.canonical_name = "gpt-4-turbo",
     .context_window = 128'000,
     .max_output_tokens = 4'096,
     .supports_tools = true,
     .supports_vision = true},
    {.canonical_name = "gpt-4",
     .context_window = 8'192,
     .max_output_tokens = 4'096,
     .supports_tools = true,
     .supports_vision = false},
    {.canonical_name = "gpt-3.5-turbo",
     .context_window = 16'385,
     .max_output_tokens = 4'096,
     .supports_tools = true,
     .supports_vision = false},

    // ============================================================
    // DeepSeek 系列
    // ============================================================
    {.canonical_name = "deepseek-chat",
     .context_window = 64'000,
     .max_output_tokens = 8'192,
     .supports_tools = true,
     .supports_vision = false},
    {.canonical_name = "deepseek-reasoner",
     .context_window = 64'000,
     .max_output_tokens = 8'192,
     .supports_tools = false,
     .supports_vision = false},
};

/// @brief 不区分大小写的子串包含检查
bool contains_ci(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    if (haystack.size() < needle.size()) return false;
    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(), needle.end(),
        [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
        });
    return it != haystack.end();
}

} // anonymous namespace

const ModelCapability* find_model_capability(std::string_view model_name) {
    if (model_name.empty()) return nullptr;

    // 1. 精确匹配（不区分大小写）
    for (const auto& cap : s_capabilities) {
        if (cap.canonical_name.size() == model_name.size() &&
            std::equal(cap.canonical_name.begin(), cap.canonical_name.end(),
                       model_name.begin(),
                       [](char a, char b) {
                           return std::tolower(static_cast<unsigned char>(a)) ==
                                  std::tolower(static_cast<unsigned char>(b));
                       })) {
            return &cap;
        }
    }

    // 2. 最长子串匹配（如 "claude-sonnet-4-5-20250929" 匹配 "claude-sonnet-4-5"）
    const ModelCapability* best = nullptr;
    size_t best_len = 0;
    for (const auto& cap : s_capabilities) {
        if (cap.canonical_name.size() > best_len &&
            contains_ci(model_name, cap.canonical_name)) {
            best = &cap;
            best_len = cap.canonical_name.size();
        }
    }
    return best;
}

int32_t get_context_window_for_model(std::string_view model_name) {
    if (auto* cap = find_model_capability(model_name)) {
        if (cap->context_window > 0) {
            return cap->context_window;
        }
    }
    return MODEL_CONTEXT_WINDOW_DEFAULT;
}

int32_t get_max_output_tokens_for_model(std::string_view model_name) {
    if (auto* cap = find_model_capability(model_name)) {
        if (cap->max_output_tokens > 0) {
            return cap->max_output_tokens;
        }
    }
    return MAX_OUTPUT_TOKENS_DEFAULT;
}

} // namespace agent
