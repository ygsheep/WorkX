# Island 灵动岛 IPC 模块（TUI 侧，供 GUI 悬浮面板消费）
# 依赖：workx_agent（HttpClient、事件总线） + workx_core（ResultV2 等）
# 边界：island 禁止 include tui/app（tests/unit/island/test_layer_boundary.cpp 校验）
# 交付：仅 TUI 进程内使用，不安装、不导出（与 workx 主程序同级）

add_library(workx_island STATIC)

target_include_directories(workx_island PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
)

target_link_libraries(workx_island PUBLIC
    workx_agent
    workx_core
)

target_sources(workx_island PRIVATE
    jsonl_protocol.cpp
    pricing_table.cpp
    cost_accumulator.cpp
    registry_writer.cpp
    island_event_bridge.cpp
    island_server.cpp
    balance_fetcher.cpp
)

if(WIN32)
    target_sources(workx_island PRIVATE ipc/transport_win32.cpp)
else()
    target_sources(workx_island PRIVATE ipc/transport_posix.cpp)
endif()
