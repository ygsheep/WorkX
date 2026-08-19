# ============================================================
# 实验 FTXUI TUI（codex）
# 与 src/tui (workx_tui) 完全隔离：不链接、不 include。
# 复用 workx_agent + workx_core（EventBus / ChatSession / ReActLoop）。
# 见 docs/plans/2026-08-17-ftxui-tui-design.md
# ============================================================

# ---- FTXUI（实验独占依赖，不进入 vcpkg manifest）----
# 使用 vendored 源码（third_party/ftxui），含 Windows 鼠标事件补丁：
# 上游 v7.0.3 在 Windows 下忽略 MOUSE_EVENT，导致点击/滚轮/文本选中全部失效。
include(FetchContent)
FetchContent_Declare(
    ftxui
    SOURCE_DIR ${CMAKE_SOURCE_DIR}/third_party/ftxui
)
FetchContent_MakeAvailable(ftxui)

add_executable(codex
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
    widgets/composer.cpp
    widgets/status_line.cpp
    widgets/suggest_panel.cpp
    widgets/search_palette.cpp
)

target_include_directories(codex PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(codex PRIVATE
    workx_agent
    workx_core
    ftxui::screen
    ftxui::dom
    ftxui::component
)

if(nlohmann_json_FOUND)
    target_link_libraries(codex PRIVATE nlohmann_json::nlohmann_json)
endif()

if(WIN32)
    add_compile_options(/utf-8)
endif()

# ============================================================
# 应用图标（复用 src/icon.*，与 workx 主程序一致）：
#   Windows: src/icon.ico → RC 资源嵌入可执行文件
#   macOS:   src/icon.icns → MACOSX_BUNDLE 资源
#   Linux:   src/icon.png → 拷贝到输出目录
# ============================================================
set(FTX_ICON_PNG "${CMAKE_CURRENT_SOURCE_DIR}/../icon.png")
set(FTX_ICON_ICO "${CMAKE_CURRENT_SOURCE_DIR}/../icon.ico")
set(FTX_ICON_ICNS "${CMAKE_CURRENT_SOURCE_DIR}/../icon.icns")

if(WIN32 AND EXISTS "${FTX_ICON_ICO}")
    set(FTX_RC_FILE "${CMAKE_CURRENT_BINARY_DIR}/codex_icon.rc")
    # 使用数字 ID（1），Windows 资源管理器自动识别 exe 中的最低 ID ICON 资源
    file(WRITE "${FTX_RC_FILE}"
        "1 ICON \"${FTX_ICON_ICO}\"\n"
    )
    target_sources(codex PRIVATE "${FTX_RC_FILE}")
    message(STATUS "Icon: codex Windows ICO embedded via RC file")
endif()

if(APPLE)
    if(EXISTS "${FTX_ICON_ICNS}")
        set_target_properties(codex PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_ICON_FILE "icon.icns"
        )
        set_source_files_properties("${FTX_ICON_ICNS}" PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources"
        )
        target_sources(codex PRIVATE "${FTX_ICON_ICNS}")
        message(STATUS "Icon: codex macOS ICNS added to bundle Resources")
    endif()
endif()

if(UNIX AND NOT APPLE AND EXISTS "${FTX_ICON_PNG}")
    add_custom_command(TARGET codex POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${FTX_ICON_PNG}"
            "$<TARGET_FILE_DIR:codex>/icon.png"
        COMMENT "Copying application icon: icon.png"
    )
    message(STATUS "Icon: codex Linux PNG copied to output dir")
endif()