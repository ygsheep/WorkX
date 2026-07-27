/**
 * @file context.cpp
 * @brief ToolContext — helper 方法实现
 * @details 将 IConfigManager 解析逻辑放在 .cpp 中，避免 context.h 强依赖
 *          i_config_manager.h / config_manager.h，降低头文件传播成本。
 * @version 1.2.0
 * @date 2026-07
 */

#include "agent/tool/context.h"
#include "core/config/i_config_manager.h"

#include <stdexcept>

namespace agent::tool {

// H-5：强制 DI 注入。nullptr 时抛异常，禁止隐式回退单例。
// 调用方（ReActLoop）必须显式传入非空 IConfigManager*。
IConfigManager& ToolContext::config_manager() const {
    if (config_manager_ptr == nullptr) {
        throw std::logic_error(
            "ToolContext::config_manager() requires non-null config_manager_ptr "
            "(H-5: ReActLoop must inject IConfigManager explicitly)");
    }
    return *config_manager_ptr;
}

} // namespace agent::tool
