# 测试构建配置

enable_testing()

find_package(Catch2 CONFIG REQUIRED)

# 测试可执行文件
set(TEST_SOURCES
    test_main.cpp
    test_config_manager.cpp
    test_command_system.cpp
    test_sse_parser.cpp
    test_chat_session.cpp
    test_provider_preset.cpp
    test_openai_adapter.cpp
    test_anthropic_adapter.cpp
    test_http_client.cpp
    test_client.cpp
    test_utf8_utils.cpp
    test_markdown_renderer.cpp
    test_display_buffer.cpp
    test_file_edit_tool.cpp
)

add_executable(workx_tests ${TEST_SOURCES})
target_compile_definitions(workx_tests PRIVATE SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(workx_tests PRIVATE libworkx Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(workx_tests
    DL_PATHS "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>"
)