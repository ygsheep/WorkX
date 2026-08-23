# 集成测试构建
# @details X-2 修复：自动启停 tests/integration/fixtures/test_server.py
#          无需手动启动 LM Studio，测试运行时自动拉起 Python 服务器
#          若设置环境变量 LM_STUDIO_BASE_URL，则切换为使用 LM Studio（向后兼容）
#
# 运行环境变量（可选）:
#   LM_STUDIO_BASE_URL=http://127.0.0.1:1234  (覆盖默认，使用 LM Studio)

set(INTEGRATION_TEST_SOURCES
    ../test_main.cpp
    test_http_client.cpp
    test_client.cpp
)

add_executable(workx_integration_tests ${INTEGRATION_TEST_SOURCES})
target_compile_definitions(workx_integration_tests PRIVATE
    SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    WORKX_TEST_SERVER_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/test_server.py"
)
target_include_directories(workx_integration_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(workx_integration_tests PRIVATE workx_agent Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(workx_integration_tests
    DL_PATHS "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>"
)
