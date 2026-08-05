# 语法高亮渲染示例
# 链接 libworkx, 直接调用 highlight_code / render_code_block
# 覆盖: 多语言代码块 + diff + FileRead 模拟行号 + 错误恢复 (半截代码)
add_executable(example_code_highlight
    main.cpp
)

target_link_libraries(example_code_highlight PRIVATE workx_tui)

# 确保启用 tree-sitter 时, example 也能感知 grammar 定义 (从 libworkx PUBLIC 继承)
# WORKX_HAS_TREE_SITTER / WORKX_TS_<NAME> 已经通过 libworkx PUBLIC 传播
