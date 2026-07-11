#pragma once

/// @brief 模型配置表
///
/// 定义各模型的静态配置信息：
/// - 模型 ID、显示名称
/// - 上下文窗口大小
/// - 最大输出 token 数
/// - 支持的能力（vision、tool_use、streaming）
/// - 定价信息（可选）
/// - 注意：provider_preset.h 中的 ProviderPreset 将逐步迁移至此
