/**
 * @file i_backend_admin.h
 * @brief 后端管理接口（D-3 接口隔离）
 * @details 从 IBackend 拆分出的管理能力：模型列表、切换、信息查询。
 *          ChatSession 仅依赖 ICompletionProvider，UI/管理层的 list_models /
 *          set_model_name 等调用通过 IBackendAdmin 解耦。
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <vector>

#include "core/utils/result.h"
#include "agent/api/backend_types.h"

namespace agent {

/// @brief 后端管理能力接口
/// @details 与 ICompletionProvider（推理能力）正交。IBackend 组合两者。
class IBackendAdmin {
public:
    virtual ~IBackendAdmin() = default;

    /// @brief 后端名称
    virtual std::string name() const = 0;

    /// @brief 从 API 获取可用模型列表
    /// @return 模型信息列表，或错误信息
    virtual Result<std::vector<ModelInfo>, std::string> list_models() = 0;

    /// @brief 运行时切换模型名（不重启）
    /// @param name 新模型名
    virtual void set_model_name(const std::string& name) = 0;

    /// @brief 获取当前模型信息
    virtual ModelInfo get_model_info() const = 0;
};

} // namespace agent
