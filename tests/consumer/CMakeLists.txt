# 外部消费者冒烟工程（Issue #21 验收）
#
# 三种消费模式：
# 1. 仓库内 smoke（由根 WORKX_BUILD_CONSUMER=ON 注入）：直接链已定义的 workx_agent
# 2. 外部 add_subdirectory 模式：单独 configure，把 workx 源码树加进来
#    cmake -S tests/consumer -B <build> -DWORKX_SOURCE_DIR=<workx 根> \
#          -DCMAKE_TOOLCHAIN_FILE=<vcpkg> -DWORKX_FETCH_GRAMMARS=OFF
# 3. 外部 find_package 模式（M2 验收）：
#    cmake -S tests/consumer -B <build> -DCMAKE_PREFIX_PATH=<workx install 前缀>
# 注意：模式 2/3 是独立 configure；模式 1 才是仓库根的子目录。

cmake_minimum_required(VERSION 3.21)
project(workx_consumer_smoke CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WORKX_CONSUMER_ADD_SUBDIRECTORY)
    # 仓库根已定义 workx_agent，直接链接即可
    set(AGENT_TARGET workx_agent)
elseif(WORKX_SOURCE_DIR)
    # 外部 add_subdirectory 模式：workx 源码树只会被加入一次，无目标重复问题。
    # option() 不覆盖已存在的普通变量，先关掉根级测试/示例避免无关目标
    set(WORKX_BUILD_TESTS OFF)
    set(WORKX_BUILD_EXAMPLES OFF)
    add_subdirectory(${WORKX_SOURCE_DIR} workx)
    set(AGENT_TARGET workx_agent)
else()
    find_package(workx CONFIG REQUIRED)
    set(AGENT_TARGET workx::agent)
endif()

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE ${AGENT_TARGET})

# workx 公共头依赖标准一致性预处理器与 utf-8 源码（与仓库根编译选项保持一致）
if(MSVC)
    target_compile_options(consumer PRIVATE /W4 /utf-8 /Zc:preprocessor)
    target_compile_definitions(consumer PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
