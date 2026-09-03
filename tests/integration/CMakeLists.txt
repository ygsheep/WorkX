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
    test_agent_skills_mcp.cpp
)

add_executable(workx_integration_tests ${INTEGRATION_TEST_SOURCES})
target_compile_definitions(workx_integration_tests PRIVATE
    SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    WORKX_TEST_SERVER_SCRIPT="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/test_server.py"
)
# 复用单元测试的 mock 辅助头（mock_provider / mock_event_bus / mock_config_manager）
target_include_directories(workx_integration_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../unit
)
target_link_libraries(workx_integration_tests PRIVATE workx_agent Catch2::Catch2WithMain)

include(Catch)
# Windows GBK 控制台下 CMake 捕获中文测试名会转码成 mojibake 导致 JSON 解析失败，
# 与单元测试一致，用本地补丁版 CatchAddTests.cmake（捕获前先 chcp 65001 切 UTF-8 代码页）。
# include(Catch) 会把变量覆写回 vcpkg 默认版，必须在其后再设一次。
set(_CATCH_DISCOVER_TESTS_SCRIPT
  ${WORKX_ROOT}/cmake/CatchAddTests_utf8.cmake
  CACHE INTERNAL "Catch2 full path to CatchAddTests.cmake helper file"
)
catch_discover_tests(workx_integration_tests
    DL_PATHS "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>"
)
