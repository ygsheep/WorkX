# 集成测试构建
# @details 依赖外部服务 (LM Studio OpenAI 兼容服务器)
#          通过 -DWORKX_BUILD_INTEGRATION_TESTS=ON 启用
#          fixture/test_server.py 是 mock HTTP 服务器 (Python), 由测试方手动启动
#
# 运行环境变量:
#   LM_STUDIO_BASE_URL=http://127.0.0.1:1234/v1  (LM Studio 默认端口)

set(INTEGRATION_TEST_SOURCES
    ../test_main.cpp
    test_http_client.cpp
    test_client.cpp
)

add_executable(workx_integration_tests ${INTEGRATION_TEST_SOURCES})
target_compile_definitions(workx_integration_tests PRIVATE SOURCE_DIR="${CMAKE_SOURCE_DIR}")
target_link_libraries(workx_integration_tests PRIVATE libworkx Catch2::Catch2WithMain)

include(Catch)
catch_discover_tests(workx_integration_tests
    DL_PATHS "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>"
)
