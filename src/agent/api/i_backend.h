/**
 * @file i_backend.h
 * @brief IBackend 接口（D-3 拆分后）
 * @details 组合 ICompletionProvider（推理能力）+ IBackendAdmin（管理能力），
 *          外加生命周期方法 initialize/shutdown/is_ready。
 *          ChatSession 仅依赖 ICompletionProvider；UI 层通过 IBackendAdmin 调用管理接口。
 * @version 3.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <memory>
#include "core/utils/result.h"
#include "agent/api/backend_types.h"
#include "agent/api/i_stream_reader.h"
#include "agent/api/i_completion_provider.h"
#include "agent/api/i_backend_admin.h"

namespace agent {

/// @brief 推理后端接口
/// @details 同时实现 ICompletionProvider（推理）和 IBackendAdmin（管理），
///          供 ChatSession 直接使用（通过 ICompletionProvider 面），
///          供 UI/Client 调用管理接口（通过 IBackendAdmin 面）。
class IBackend : public ICompletionProvider, public IBackendAdmin {
public:
    /// @brief 初始化后端
    /// @param config 后端配置
    virtual Result<void, std::string> initialize(const BackendConfig& config) = 0;

    /// @brief 关闭后端
    virtual void shutdown() = 0;

    /// @brief 后端是否就绪
    virtual bool is_ready() const = 0;
};

} // namespace agent
