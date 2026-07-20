# 单元测试构建
# @details 与 src/ 模块对齐分层 (agent/core/tui), 仅依赖 libworkx, 无外部服务

set(UNIT_TEST_SOURCES
    ../test_main.cpp
    agent/api/test_anthropic_adapter.cpp
    agent/api/test_openai_adapter.cpp
    agent/api/test_sse_parser.cpp
    agent/core/test_chat_session.cpp
    agent/command/test_command_system.cpp
    agent/model/test_provider_preset.cpp
    agent/tool/test_file_edit_tool.cpp
    core/config/test_config_manager.cpp
    tui/core/test_display_buffer.cpp
    tui/render/test_markdown_renderer.cpp
    tui/utils/test_utf8_utils.cpp
)

add_executable(workx_unit_tests ${UNIT_TEST_SOURCES})
target_compile_definitions(workx_unit_tests PRIVATE SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(workx_unit_tests PRIVATE libworkx Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(workx_unit_tests
    DL_PATHS "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>"
)
