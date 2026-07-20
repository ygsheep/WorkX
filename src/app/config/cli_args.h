/**
 * @file cli_args.h
 * @brief CLI 参数解析
 * @details 解析命令行参数（--provider, --model, --api-key 等）
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

namespace agent {

/// @brief CLI 参数解析（最高优先级，覆盖配置文件和环境变量）
void parse_cli_args(int argc, char* argv[]);

} // namespace agent
