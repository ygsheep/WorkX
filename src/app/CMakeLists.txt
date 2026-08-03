# 应用层：command / config / ui / factory
# main.cpp 由 src/CMakeLists.txt 直接加入 workx 可执行文件

target_sources(libworkx PRIVATE
    command/builtin_commands.cpp
    config/app_config.cpp
    config/cli_args.cpp
    factory.cpp
    session_restore.cpp
    ui/model_selector.cpp
    ui/path_completer.cpp
    ui/file_index.cpp
    ui/provider_form.cpp
)
