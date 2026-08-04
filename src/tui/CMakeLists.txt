# TUI 层：core / input / render / setup / utils / widgets

target_sources(libworkx PRIVATE
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
    target_sources(libworkx PRIVATE core/platform/platform_win32.cpp)
else()
    target_sources(libworkx PRIVATE core/platform/platform_posix.cpp)
endif()
