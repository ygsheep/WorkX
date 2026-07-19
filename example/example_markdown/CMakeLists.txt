# Markdown -> TUI 字符画渲染示例
# 仅编译纯函数依赖，避免引入 CURL/logger 等重依赖
# 注: 此处不定义 WORKX_HAS_TREE_SITTER, syntax_highlighter.cpp 编译为 no-op,
#     代码块渲染仍可工作但不高亮; 主程序 libworkx 启用 tree-sitter 时才有高亮
add_executable(example_markdown
    main.cpp
    ${CMAKE_SOURCE_DIR}/src/tui/render/markdown_renderer.cpp
    ${CMAKE_SOURCE_DIR}/src/tui/render/syntax_highlighter.cpp
    ${CMAKE_SOURCE_DIR}/src/tui/utils/utf8_utils.cpp
)

target_include_directories(example_markdown PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)
