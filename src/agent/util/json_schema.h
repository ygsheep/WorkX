#pragma once

/// @brief JSON Schema 轻量验证
///
/// 对工具参数进行 JSON Schema 验证：
/// - 支持 type、properties、required 等基本关键字
/// - 类型检查（string、number、boolean、array、object）
/// - 必填字段验证
/// - 枚举值验证
/// - 不依赖外部库，轻量实现
