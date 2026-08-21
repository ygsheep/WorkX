# ============================================================
# workx 主程序（FTXUI 双栏 TUI）
# 复用 workx_agent + workx_core（EventBus / ChatSession / ReActLoop）。
# ============================================================

# ---- FTXUI（vendored 源码，含 Windows 鼠标事件补丁）----
include(FetchContent)
FetchContent_Declare(
    ftxui
    SOURCE_DIR ${CMAKE_SOURCE_DIR}/third_party/ftxui
)
FetchContent_MakeAvailable(ftxui)

add_executable(workx
    main.cpp
    app.cpp
    crash_reporter.cpp
    command/builtins.cpp
    vm/message_node.cpp
    vm/view_model.cpp
    bridge/action_queue.cpp
    bridge/event_bridge.cpp
    render/markdown_to_elements.cpp
    render/syntax_highlight.cpp
    widgets/sidebar.cpp
    widgets/sidebar_tabs.cpp
    widgets/file_viewer.cpp
    widgets/change_viewer.cpp
    widgets/composer.cpp
    widgets/status_line.cpp
    widgets/suggest_panel.cpp
    widgets/search_palette.cpp
)

target_include_directories(workx PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(workx PRIVATE
    workx_agent
    workx_core
    ftxui::screen
    ftxui::dom
    ftxui::component
)

if(nlohmann_json_FOUND)
    target_link_libraries(workx PRIVATE nlohmann_json::nlohmann_json)
endif()

if(WIN32)
    add_compile_options(/utf-8)
endif()
