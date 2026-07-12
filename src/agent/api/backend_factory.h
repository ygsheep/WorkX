/**
 * @file backend_factory.h
 * @brief 后端工厂
 * @version 2.0.0
 * @date 2026-07
 */

#pragma once

#include <memory>
#include "agent/api/chat_types.h"

namespace agent {

class IBackend;

/// @brief 后端工厂
/// @details 根据 BackendConfig 创建对应的后端实例
class BackendFactory {
public:
    /// @brief 创建后端
    /// @param config 后端配置
    /// @return 后端实例，失败返回 nullptr
    static std::unique_ptr<IBackend> create(const BackendConfig& config);
};

} // namespace agent
