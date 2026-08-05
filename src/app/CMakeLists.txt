# 应用层：command / config / ui / factory
# 定义 workx_app 静态库（宿主装配层；main.cpp 由 src/CMakeLists.txt 加入 workx 可执行文件）
# workx_app 为内部目标，不安装（外部消费面是 workx_agent）

add_library(workx_app STATIC)

target_link_libraries(workx_app PUBLIC
    workx_tui
)

target_sources(workx_app PRIVATE
    command/builtin_commands.cpp
    command/skill_commands.cpp
    config/cli_args.cpp
    factory.cpp
    session_restore.cpp
    ui/model_selector.cpp
    ui/path_completer.cpp
    ui/provider_form.cpp
)
