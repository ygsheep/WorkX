# 单元测试构建
# @details 按模块拆分为多个独立测试可执行文件，解决两大痛点：
#   1. 编译慢：单一二进制需全量重编译+重链接；拆分后改动某模块只重编/重链该模块目标，
#      且多个测试目标可并行编译（cmake --build -j N）。
#   2. 运行慢：单一进程串行跑 919 个用例；拆分后各模块可经 ctest -j N 并行运行。
#
# 模块划分（与 src/ 分层对齐，每个目标只链接其依赖的最少库）：
#   - core_unit_tests    : tests/unit/core/**    -> workx_core
#   - agent_unit_tests   : tests/unit/agent/**   -> workx_agent (含 helpers 自测)
#   - tui_unit_tests     : tests/unit/tui/**     -> workx_tui
#   - island_unit_tests  : tests/unit/island/**  -> workx_island
#   - app_unit_tests     : tests/unit/app/**     -> workx_app
#
# 常用命令：
#   - 只构建某模块测试：  cmake --build build --target <模块>_unit_tests -j <N>
#   - 只运行某模块测试：  ./build/bin/Release/<模块>_unit_tests.exe
#   - 并行运行所有测试：  ctest --test-dir build -C Release -j <N>
#   - 快速回归（跳过 [slow] 慢测试）：  ctest --test-dir build -C Release -LE slow -j <N>
#   - 只跑慢测试（验证超时/并发类逻辑）： ctest --test-dir build -C Release -L slow -j <N>
#   - 按功能标签过滤：        ctest --test-dir build -C Release -L <tag> -j <N>

include(Catch)

# Catch2 提供 main()；每个目标都需这一个编译占位
set(UNIT_TEST_COMMON ../test_main.cpp)

# 按模块用 glob 收集测试源文件（CONFIGURE_DEPENDS：新增测试文件自动纳入，无需手改列表）
file(GLOB_RECURSE CORE_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/core/*.cpp)
file(GLOB_RECURSE AGENT_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/agent/*.cpp)
file(GLOB_RECURSE TUI_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tui/*.cpp)
file(GLOB_RECURSE ISLAND_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/island/*.cpp)
file(GLOB_RECURSE APP_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/app/*.cpp)
# helpers 自测（Mock 接口实现，依赖 agent 接口），归入 agent 目标
file(GLOB HELPER_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/helpers/*.cpp)

# test_command_system 物理位于 agent/command/，但依赖 app 层 register_system_commands
# （builtin_commands.cpp），须归入 app 目标才能链接该符号
list(REMOVE_ITEM AGENT_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/agent/command/test_command_system.cpp)
list(APPEND APP_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/agent/command/test_command_system.cpp)

# 为某模块创建独立测试可执行文件并注册到 ctest
# 用法: workx_add_unit_target(<target> <链接库...> <源文件...>)
function(workx_add_unit_target target)
    list(GET ARGN 0 _lib)
    list(REMOVE_AT ARGN 0)

    add_executable(${target} ${UNIT_TEST_COMMON} ${ARGN})
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(${target} PRIVATE SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    target_link_libraries(${target} PRIVATE ${_lib} Catch2::Catch2WithMain)

    # ADD_TAGS_AS_LABELS：把每个测试的 Catch2 标签（含 [slow]）映射为 ctest 标签，
    # 从而支持 ctest -L/-LE 按标签过滤（如 ctest -LE slow 跑快速子集）
    catch_discover_tests(${target}
        ADD_TAGS_AS_LABELS ON
        DL_PATHS "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>"
    )
endfunction()

workx_add_unit_target(core_unit_tests   workx_core   ${CORE_TEST_SOURCES})
workx_add_unit_target(agent_unit_tests  workx_agent  ${AGENT_TEST_SOURCES} ${HELPER_TEST_SOURCES})
workx_add_unit_target(tui_unit_tests    workx_tui    ${TUI_TEST_SOURCES})
workx_add_unit_target(island_unit_tests workx_island ${ISLAND_TEST_SOURCES})
workx_add_unit_target(app_unit_tests    workx_app    ${APP_TEST_SOURCES})

# ---- ftxtui 无头单测（H1 修复：为实验 FTXUI TUI 建立回归网）----
# 仅编译不依赖 ScreenInteractive 的无头源（view_model/message_node/markdown/
# command_registry），避免在测试链接中拖动整个 FTXUI 交互栈。
# 依赖 codex 目标通过 WORKX_BUILD_FTX_TUI 打开的 ftxui::* imported targets。
file(GLOB_RECURSE FTXTUI_TEST_SOURCES
     CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/ftxtui/*.cpp)
if(WORKX_BUILD_FTX_TUI AND FTXTUI_TEST_SOURCES)
    add_executable(ftxtui_unit_tests
        ${UNIT_TEST_COMMON}
        ${FTXTUI_TEST_SOURCES}
        # 被测量 FTXUI 无头实现（直接编译源文件；与 codex 目标共享源码）
        ${WORKX_ROOT}/src/ftxtui/render/markdown_to_elements.cpp
        ${WORKX_ROOT}/src/ftxtui/render/syntax_highlight.cpp
        ${WORKX_ROOT}/src/ftxtui/vm/message_node.cpp
        ${WORKX_ROOT}/src/ftxtui/vm/view_model.cpp
        ${WORKX_ROOT}/src/ftxtui/command/command_registry.cpp
    )
    target_include_directories(ftxtui_unit_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${WORKX_ROOT}/src
        ${WORKX_ROOT}/src/ftxtui
    )
    target_compile_definitions(ftxtui_unit_tests PRIVATE
        SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    target_link_libraries(ftxtui_unit_tests PRIVATE
        workx_agent
        workx_core
        ftxui::dom
        ftxui::screen
        Catch2::Catch2WithMain
    )
    if(nlohmann_json_FOUND)
        target_link_libraries(ftxtui_unit_tests PRIVATE nlohmann_json::nlohmann_json)
    endif()
    catch_discover_tests(ftxtui_unit_tests
        ADD_TAGS_AS_LABELS ON
        DL_PATHS "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/$<CONFIG>"
    )
endif()