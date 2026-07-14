# Workx TUI chat client build configuration
# @date 2026-07

cmake_minimum_required(VERSION 3.21)
project(workx VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
if(MSVC)
    add_compile_options(/W4 /utf-8 /Zc:preprocessor)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(nlohmann_json CONFIG QUIET)

find_package(CURL CONFIG REQUIRED)

set(WORKX_SOURCES
    src/core/task/task_manager.cpp
    src/agent/api/sse_parser.cpp
    src/core/config/config_manager.cpp
    src/tui/core/terminal.cpp
    src/tui/input/line_editor.cpp
    src/tui/core/color_scheme.cpp
    src/tui/render/chat_renderer.cpp
    src/tui/render/output_formatter.cpp
    src/tui/render/streaming_buffer.cpp
    src/tui/widgets/status_bar.cpp
    src/tui/widgets/command_panel.cpp
    src/tui/widgets/file_search_panel.cpp
    src/tui/widgets/select_panel.cpp
    src/tui/widgets/bottom_bar_manager.cpp
    src/tui/setup/setup_wizard.cpp
    src/tui/core/screen.cpp
    src/tui/core/display_buffer.cpp
    src/tui/utils/utf8_utils.cpp
    src/tui/render/markdown_renderer.cpp
    src/tui/render/spinner.cpp
    src/tui/input/history.cpp
    src/agent/core/chat_session.cpp
    src/agent/api/backend_factory.cpp
    src/agent/api/client.cpp
    src/agent/api/remote/remote_backend.cpp
    src/agent/api/remote/sse_stream_reader.cpp
    src/agent/api/remote/http_client.cpp
    src/agent/model/provider_preset.cpp
    src/agent/api/provider/openai_adapter.cpp
    src/agent/api/provider/anthropic_adapter.cpp
    src/agent/command/source/registry.cpp
    src/agent/command/source/executor.cpp
    src/agent/tool/encoding.cpp
    src/agent/tool/FileReadTool/file_read_tool.cpp
    src/agent/tool/FileEditTool/file_edit_tool.cpp
    src/agent/tool/FileWriteTool/file_write_tool.cpp
    src/agent/tool/GlobTool/glob_tool.cpp
    src/agent/tool/GrepTool/grep_tool.cpp
    src/agent/tool/BashTool/bash_tool.cpp
    src/agent/tool/AgentTool/agent_tool.cpp
    src/agent/tool/MCPTool/mcp_tool.cpp
    src/agent/tool/WebFetchTool/web_fetch_tool.cpp
    src/app/command/builtin_commands.cpp
    src/app/config/app_config.cpp
    src/app/config/cli_args.cpp
    src/app/ui/model_selector.cpp
    src/app/ui/path_completer.cpp
    src/app/ui/file_index.cpp
    src/agent/input/parser.h
    src/agent/input/processor.h
    src/agent/input/types.h
)

# Add platform-specific source
if(WIN32)
    list(APPEND WORKX_SOURCES src/tui/core/platform/platform_win32.cpp)
else()
    list(APPEND WORKX_SOURCES src/tui/core/platform/platform_posix.cpp)
endif()

add_library(libworkx STATIC ${WORKX_SOURCES})

# end modification: platform-specific sources handled conditionally
target_include_directories(libworkx PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)

target_compile_definitions(libworkx PUBLIC
    WORKX_VERSION="${PROJECT_VERSION}"
)

if(nlohmann_json_FOUND)
    target_link_libraries(libworkx PUBLIC nlohmann_json::nlohmann_json)
    target_compile_definitions(libworkx PUBLIC WORKX_HAS_NLOHMANN_JSON)
endif()

target_link_libraries(libworkx PUBLIC
    CURL::libcurl
    logger
 )
target_compile_definitions(libworkx PUBLIC WORKX_HAS_CURL)

add_subdirectory(lib/liblogger)

include_directories(${CMAKE_SOURCE_DIR}/lib)

add_executable(workx src/app/main.cpp)
target_link_libraries(workx PRIVATE libworkx)

option(WORKX_BUILD_TESTS "Build tests" ON)
if(WORKX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

option(WORKX_BUILD_EXAMPLES "Build examples" ON)
if(WORKX_BUILD_EXAMPLES)
    add_subdirectory(example/example_markdown)
endif()