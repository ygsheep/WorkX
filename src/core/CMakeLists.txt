# 核心基础模块：config / events / task / utils
# 仅头文件接口 + 少量实现，无外部依赖

target_sources(libworkx PRIVATE
    config/config_manager.cpp
    task/task_manager.cpp
    utils/error.cpp
)
