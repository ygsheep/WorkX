# 单元测试构建
# @details 与 src/ 模块对齐分层 (agent/core/tui), 仅依赖 libworkx, 无外部服务

set(UNIT_TEST_SOURCES
    ../test_main.cpp
    helpers/test_mock_helpers.cpp
    app/test_factory.cpp
    app/test_cli_args.cpp
    agent/api/test_anthropic_adapter.cpp
    agent/api/test_openai_adapter.cpp
    agent/api/test_http_response.cpp
    agent/api/test_remote_backend.cpp
    agent/api/test_retry_policy.cpp
    agent/api/test_sse_parser.cpp
    agent/api/test_client.cpp
    agent/core/test_chat_session.cpp
    agent/core/test_react_loop.cpp
    agent/command/test_command_system.cpp
    agent/model/test_provider_preset.cpp
    agent/model/test_model_config.cpp
    agent/compact/test_token_count.cpp
    agent/input/test_input_processor.cpp
    agent/tool/test_file_edit_tool.cpp
    agent/tool/test_tool_context.cpp
    agent/tool/test_tool_executor.cpp
    agent/tool/test_tool_registry.cpp
    core/config/test_config_manager.cpp
    core/events/test_event_bus.cpp
    core/process/test_subprocess.cpp
    core/process/test_tool_registry.cpp
    core/process/test_sandbox.cpp
    core/task/test_task_manager.cpp
    core/task/test_thread_pool.cpp
    core/test_tool_kind.cpp
    core/utils/test_error.cpp
    core/utils/test_result_v2.cpp
    tui/core/test_display_buffer.cpp
    tui/core/test_terminal_overlay.cpp
    tui/core/test_tui_state.cpp
    tui/model/test_token_stats_model.cpp
    tui/model/test_tool_call_tracker.cpp
    tui/render/test_markdown_renderer.cpp
    tui/render/test_spinner.cpp
    tui/render/test_streaming_buffer.cpp
    tui/utils/test_utf8_utils.cpp
    tui/widgets/test_status_bar.cpp
)

add_executable(workx_unit_tests ${UNIT_TEST_SOURCES})
target_include_directories(workx_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(workx_unit_tests PRIVATE SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(workx_unit_tests PRIVATE libworkx Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(workx_unit_tests
    DL_PATHS "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>"
)
