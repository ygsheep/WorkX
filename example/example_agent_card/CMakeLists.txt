# Agent Tools 子代理卡片 TUI 示例（边框流动动画）
# 边框风格对齐 example_provider_form（圆角框），运行态绿色流动边框
add_executable(example_agent_card
    main.cpp
)

target_link_libraries(example_agent_card PRIVATE workx_app)