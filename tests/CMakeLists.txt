# 测试构建总入口
# @details 按测试类型拆分为 unit / integration 两层:
#   - unit: 纯单元测试, 无外部依赖, 默认构建
#   - integration: 需要外部服务 (LM Studio / mock server), 通过 option 控制

enable_testing()

find_package(Catch2 CONFIG REQUIRED)

# 单元测试: 默认构建
add_subdirectory(unit)

# 集成测试: 默认 OFF, 避免 CI 因找不到 LM Studio 而失败
option(WORKX_BUILD_INTEGRATION_TESTS "Build integration tests (require external services like LM Studio)" OFF)
if(WORKX_BUILD_INTEGRATION_TESTS)
    add_subdirectory(integration)
endif()
