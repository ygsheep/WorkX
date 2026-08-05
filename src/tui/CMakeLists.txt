# TUI 层：core / input / render / setup / utils / widgets
# 定义 workx_tui 静态库（内部目标，不安装；依赖 workx_agent + tree-sitter）

add_library(workx_tui STATIC)

target_include_directories(workx_tui PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
)

# 第三方依赖（PUBLIC：公共头暴露 nlohmann_json；tree-sitter runtime 亦 PUBLIC）
if(nlohmann_json_FOUND)
    target_link_libraries(workx_tui PUBLIC nlohmann_json::nlohmann_json)
endif()

target_link_libraries(workx_tui PUBLIC
    workx_agent
)

# Tree-sitter（可选，仅 TUI 语法高亮使用；由根 CMakeLists.txt 探测后传入变量）
if(WORKX_HAS_TREE_SITTER)
    target_link_libraries(workx_tui PUBLIC ${WORKX_TS_RUNTIME_LIB})
    target_compile_definitions(workx_tui PUBLIC WORKX_HAS_TREE_SITTER)
    if(WORKX_TS_GRAMMAR_LIBS)
        target_link_libraries(workx_tui PRIVATE ${WORKX_TS_GRAMMAR_LIBS})
        # grammar 定义 PUBLIC：example 等链接方需感知 WORKX_TS_<NAME>
        target_compile_definitions(workx_tui PUBLIC ${WORKX_TS_GRAMMAR_DEFS})
    endif()
endif()

target_sources(workx_tui PRIVATE
    # core
    core/terminal.cpp
    core/color_scheme.cpp
    core/screen.cpp
    core/display_buffer.cpp
    core/platform/vt_input_decoder.cpp

    # input
    input/line_editor.cpp
    input/history.cpp

    # render
    render/chat_renderer.cpp
    render/session_log.cpp
    render/output_formatter.cpp
    render/streaming_buffer.cpp
    render/markdown_renderer.cpp
    render/spinner.cpp
    render/syntax_highlighter.cpp

    # setup
    setup/setup_wizard.cpp

    # utils
    utils/utf8_utils.cpp

    # widgets
    widgets/status_bar.cpp
    widgets/command_panel.cpp
    widgets/file_search_panel.cpp
    widgets/select_panel.cpp
    widgets/bottom_bar_manager.cpp
    widgets/session_picker.cpp
    widgets/choice_panel.cpp
)

# 平台特定实现
if(WIN32)
    target_sources(workx_tui PRIVATE core/platform/platform_win32.cpp)
else()
    target_sources(workx_tui PRIVATE core/platform/platform_posix.cpp)
endif()
