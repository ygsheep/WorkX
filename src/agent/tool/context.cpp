/**
 * @file context.cpp
 * @brief ToolContext — helper 方法实现
 * @details 将 IConfigManager 解析逻辑放在 .cpp 中，避免 context.h 强依赖
 *          i_config_manager.h / config_manager.h，降低头文件传播成本。
 * @version 1.1.0
 * @date 2026-07
 */

#include "agent/tool/context.h"
#include "core/config/i_config_manager.h"
#include "core/config/config_manager.h"

namespace agent::tool {

// D-5：依赖解析（nullptr 时回退单例，向后兼容）
IConfigManager& ToolContext::config_manager() const {
    return config_manager_ptr ? *config_manager_ptr : ConfigManager::instance();
}

} // namespace agent::tool
