/**
 * @file system_events.h
 * @brief 系统事件类型（H-10：从 events.h 按域拆分）
 * @details 跨切面系统事件：模型加载、后端状态、应用关闭。
 *          订阅方按需 include 本文件，避免引入流式/Agent 事件。
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>

namespace agent {

/// @brief 模型加载进度
struct ModelLoadEvent {
    std::string model_name;
    float progress = 0.0f;
    bool complete = false;
    std::string error;
};

/// @brief 后端连接状态
struct BackendStatusEvent {
    enum Status { Disconnected, Connecting, Connected, Error } status = Disconnected;
    std::string backend_name;
    std::string error;
};

/// @brief 应用关闭请求
struct ShutdownEvent {
    bool force = false;
};

} // namespace agent
