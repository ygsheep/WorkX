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

# 根 CMakeLists 把 /we4700-4703（MSVC 告警转错误）作为第一方代码策略，但会
# 继承到 vendored 第三方 ftxui，使其 C4702「不可达代码」等告警变成编译错误
# （screen/terminal.cpp、component/event.cpp 等）。为 ftxui 三个库目标统一
# 强制包含抑制头，在 TU 最顶端禁用这些告警，且不改动 ftxui 源码。
if(MSVC)
    foreach(_ftxui_target screen dom component)
        if(TARGET ${_ftxui_target})
            target_compile_options(${_ftxui_target} PRIVATE
                "/FI${CMAKE_SOURCE_DIR}/third_party/ftxui/suppress_msvc_warnings.h")
        endif()
    endforeach()
endif()

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
    render/code_card.cpp
    render/syntax_highlight.cpp
    widgets/sidebar.cpp
    widgets/sidebar_tabs.cpp
    widgets/file_viewer.cpp
    widgets/change_viewer.cpp
    widgets/project_tree.cpp
    widgets/composer.cpp
    widgets/input_history.cpp
    widgets/status_line.cpp
    widgets/suggest_panel.cpp
    widgets/search_palette.cpp
    widgets/provider_manager.cpp
    wizard.cpp
)

# ============================================================
# workx 目标的应用图标与运行时资源打包
# 注意：add_custom_command(TARGET workx POST_BUILD ...) 必须在 workx
#       目标所在的目录调用（CMake 限制）。源/目标路径变量由根
#       CMakeLists.txt / src/CMakeLists.txt 计算并通过普通变量传递。
# ============================================================

# --- Windows：通过 RC 资源文件嵌入 ICO ---
if(WIN32 AND EXISTS "${WORKX_ICON_ICO}")
    set(WORKX_RC_FILE "${CMAKE_CURRENT_BINARY_DIR}/workx_icon.rc")
    # 使用数字 ID（1），Windows 资源管理器自动识别 exe 中的最低 ID ICON 资源
    file(WRITE "${WORKX_RC_FILE}"
        "1 ICON \"${WORKX_ICON_ICO}\"\n"
    )
    target_sources(workx PRIVATE "${WORKX_RC_FILE}")
    message(STATUS "Icon: Windows ICO embedded via RC file")
endif()

# --- macOS：设置 Bundle 图标 ---
if(APPLE)
    if(EXISTS "${WORKX_ICON_ICNS}")
        set_target_properties(workx PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_ICON_FILE "icon.icns"
        )
        set_source_files_properties("${WORKX_ICON_ICNS}" PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources"
        )
        target_sources(workx PRIVATE "${WORKX_ICON_ICNS}")
        message(STATUS "Icon: macOS ICNS added to bundle Resources")
    else()
        message(WARNING "macOS build without icon: ${WORKX_ICON_ICNS} missing")
    endif()
endif()

# --- Linux：拷贝 PNG 到输出目录 ---
if(UNIX AND NOT APPLE AND EXISTS "${WORKX_ICON_PNG}")
    add_custom_command(TARGET workx POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${WORKX_ICON_PNG}"
            "$<TARGET_FILE_DIR:workx>/icon.png"
        COMMENT "Copying application icon: icon.png"
    )
    message(STATUS "Icon: Linux PNG copied to output dir")
endif()

# --- vendor/ripgrep：构建时拷贝到 workx 输出目录的 tools/ 子目录 ---
# 二进制由 scripts/fetch-ripgrep.py 下载到 vendor/ripgrep/<platform>/
# 运行时由 ToolRegistry 从 <exe_dir>/tools/rg 查找
# 缺失时自动回退 C++ 标准库实现，不报错
if(WORKX_HAS_VENDOR_RIPGREP)
    add_custom_command(TARGET workx POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:workx>/tools"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${WORKX_RG_SOURCE}"
            "$<TARGET_FILE_DIR:workx>/tools/${WORKX_RG_DEST_NAME}"
        COMMENT "Bundling ripgrep: ${WORKX_RG_DEST_NAME}"
    )
    target_compile_definitions(workx PRIVATE WORKX_HAS_BUNDLED_RIPGREP=1)
endif()

# --- vendor/jq：拷贝单文件 jq 到 <exe_dir>/tools/jq ---
# 由 scripts/fetch-jq.py 下载；运行时由 ToolRegistry::resolve_jq() 查找
if(WORKX_HAS_VENDOR_JQ)
    add_custom_command(TARGET workx POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:workx>/tools"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${WORKX_JQ_SOURCE}"
            "$<TARGET_FILE_DIR:workx>/tools/${WORKX_JQ_DEST_NAME}"
        COMMENT "Bundling jq: ${WORKX_JQ_DEST_NAME}"
    )
    target_compile_definitions(workx PRIVATE WORKX_HAS_BUNDLED_JQ=1)
endif()

target_include_directories(workx PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(workx PRIVATE
    workx_agent
    workx_core
    workx_island
    ftxui::screen
    ftxui::dom
    ftxui::component
)

if(nlohmann_json_FOUND)
    target_link_libraries(workx PRIVATE nlohmann_json::nlohmann_json)
endif()

# ---- Tree-sitter 语法高亮（可选）----
# WORKX_TS_GRAMMAR_LIBS/DEFS 由根 CMakeLists.txt 收集（FetchContent 或 vcpkg），
# 这里统一链接到 workx 目标；WORKX_TS_<NAME> 宏用于语法高亮器按需注册 grammar。
if(WORKX_HAS_TREE_SITTER)
    target_link_libraries(workx PRIVATE ${WORKX_TS_RUNTIME_LIB} ${WORKX_TS_GRAMMAR_LIBS})
    target_compile_definitions(workx PRIVATE WORKX_HAS_TREE_SITTER=1 ${WORKX_TS_GRAMMAR_DEFS})
    message(STATUS "workx: tree-sitter syntax highlighting enabled (${WORKX_TS_GRAMMAR_LIBS})")
endif()

if(WIN32)
    add_compile_options(/utf-8)
endif()
