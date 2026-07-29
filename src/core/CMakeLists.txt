# 核心基础模块：config / events / task / utils / process
# 仅头文件接口 + 少量实现，无外部依赖

target_sources(libworkx PRIVATE
    config/config_manager.cpp
    process/subprocess.cpp
    process/tool_registry.cpp
    task/task_manager.cpp
    tool_kind.cpp
    utils/error.cpp
)
