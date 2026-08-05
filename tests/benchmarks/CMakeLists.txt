# 性能基准测试构建（Q-2）
# @details 独立可执行文件 workx_benchmarks，默认不编译
#          通过 -DWORKX_BUILD_BENCHMARKS=ON 启用
#
# 运行方式：
#   cmake -B build -DWORKX_BUILD_BENCHMARKS=ON
#   cmake --build build --target workx_benchmarks
#   ./build/bin/workx_benchmarks [benchmark]
#
# 标签过滤：
#   ./build/bin/workx_benchmarks [benchmark]            # 全部基准
#   ./build/bin/workx_benchmarks [benchmark][event_bus]  # 仅 EventBus
#   ./build/bin/workx_benchmarks [benchmark][token_count] # 仅 Token 估算
#   ./build/bin/workx_benchmarks [benchmark][config]    # 仅 ConfigManager

set(BENCHMARK_SOURCES
    bench_main.cpp
    bench_event_bus.cpp
    bench_token_count.cpp
    bench_config_manager.cpp
)

add_executable(workx_benchmarks ${BENCHMARK_SOURCES})
target_include_directories(workx_benchmarks PRIVATE ${CMAKE_SOURCE_DIR}/tests/unit)
target_link_libraries(workx_benchmarks PRIVATE workx_agent Catch2::Catch2 Catch2::Catch2WithMain)

# 基准测试不参与 catch_discover_tests（避免 ctest 默认运行时拖慢 CI）
# 需手动运行可执行文件
