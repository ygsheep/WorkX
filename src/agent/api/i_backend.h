/**
 * @file i_backend.h
 * @brief IBackend 接口
 * @details 推理后端的抽象接口，同时实现 ICompletionProvider
 * @version 2.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <memory>
#include "core/utils/result.h"
#include "agent/api/chat_types.h"
#include "agent/api/i_stream_reader.h"
#include "agent/api/i_completion_provider.h"

namespace agent {

/// @brief 推理后端接口
/// @details 同时实现 ICompletionProvider，可供 ChatSession 直接使用
class IBackend : public ICompletionProvider {
public:
    /// @brief 后端名称
    virtual std::string name() const = 0;

    /// @brief 初始化后端
    /// @param config 后端配置
    virtual Result<void, std::string> initialize(const BackendConfig& config) = 0;

    /// @brief 关闭后端
    virtual void shutdown() = 0;

    /// @brief 后端是否就绪
    virtual bool is_ready() const = 0;

    /// @brief 获取模型信息
    virtual ModelInfo get_model_info() const = 0;

    /// @brief 从 API 获取可用模型列表
    /// @return 模型信息列表，或错误信息
    /// @details 默认实现返回 "Not supported"。子类如 RemoteBackend 应覆写。
    virtual Result<std::vector<ModelInfo>, std::string> list_models() {
        return Result<std::vector<ModelInfo>, std::string>::err("Not supported");
    }

    /// @brief 运行时切换模型名（不重启）
    /// @param name 新模型名
    virtual void set_model_name(const std::string& /*name*/) {}
};

} // namespace agent
