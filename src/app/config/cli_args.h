/**
 * @file cli_args.h
 * @brief CLI 参数解析
 * @details 解析命令行参数（--provider, --model, --api-key 等）
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

namespace agent {

class IConfigManager;

/// @brief CLI 参数解析（最高优先级，覆盖配置文件和环境变量）
/// @note M-2：接收 IConfigManager& 注入，可注入 Mock 测试参数解析逻辑。
void parse_cli_args(IConfigManager& cfg, int argc, char* argv[]);

} // namespace agent
