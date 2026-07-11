# Markdown -> TUI 字符画渲染示例
# 仅编译纯函数依赖，避免引入 CURL/logger 等重依赖
add_executable(example_markdown
    main.cpp
    ${CMAKE_SOURCE_DIR}/src/tui/render/markdown_renderer.cpp
    ${CMAKE_SOURCE_DIR}/src/tui/utils/utf8_utils.cpp
)

target_include_directories(example_markdown PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
