# Agent 层：api / command / compact / config / core / input / message / model / tool
# 定义 workx_agent 静态库（Agent Harness：ReAct 循环 + 工具运行时，宿主无关）

add_library(workx_agent STATIC)

# 导出名不带 workx_ 前缀（安装树中为 workx::agent，避免 workx::workx_agent 重复命名）
set_target_properties(workx_agent PROPERTIES EXPORT_NAME agent)

target_include_directories(workx_agent PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<INSTALL_INTERFACE:include>
)

target_compile_definitions(workx_agent PUBLIC
    WORKX_VERSION="${PROJECT_VERSION}"
    WORKX_BUILD_INFO="${WORKX_BUILD_INFO}"
)
if(WORKX_FILE_VERSION)
    target_compile_definitions(workx_agent PUBLIC
        WORKX_FILE_VERSION="${WORKX_FILE_VERSION}"
    )
endif()

# 第三方依赖（PUBLIC：公共头暴露 nlohmann_json / CURL / logger）
# nlohmann_json 必选（根 CMakeLists 已 find_package REQUIRED），无条件链接
target_link_libraries(workx_agent PUBLIC
    nlohmann_json::nlohmann_json
    workx_core
    CURL::libcurl
    logger
)
target_compile_definitions(workx_agent PUBLIC WORKX_HAS_CURL)

target_sources(workx_agent PRIVATE
    # config
    config/app_config.cpp

    # api
    api/sse_parser.cpp
    api/backend_factory.cpp
    api/client.cpp
    api/remote/remote_backend.cpp
    api/remote/sse_stream_reader.cpp
    api/remote/http_client.cpp
    api/provider/openai_adapter.cpp
    api/provider/anthropic_adapter.cpp

    # command (source 实现；inclaude/ 仅头文件)
    command/source/registry.cpp
    command/source/executor.cpp

    # skill
    skill/source/frontmatter.cpp
    skill/source/skill_loader.cpp
    skill/source/conditional.cpp
    skill/source/hooks.cpp

    # compact
    compact/token_count.cpp
    compact/prefix_shape.cpp
    compact/cache_aware_compactor.cpp
    compact/tool_result_maintainer.cpp
    compact/sha256.cpp

    # core
    core/chat_session.cpp
    core/react_loop.cpp

    # session
    session/session_store.cpp

    # model
    model/provider_preset.cpp
    model/config.cpp
    model/context_resolver.cpp
    model/model_catalog.cpp

    # tool: 通用工具
    tool/context.cpp
    tool/encoding.cpp
    tool/path_expand.cpp
    tool/path_matcher.cpp
    tool/path_validator.cpp
    tool/permission_ask.cpp
    tool/secret_scanner.cpp
    tool/line_endings.cpp
    tool/quote_normalizer.cpp
    tool/file_history.cpp

    # tool: 各工具实现
    tool/FileReadTool/file_read_tool.cpp
    tool/FileEditTool/file_edit_tool.cpp
    tool/FileWriteTool/file_write_tool.cpp
    tool/FileWriteTool/diff.cpp
    tool/FileReadState/file_read_state.cpp
    tool/GlobTool/glob_tool.cpp
    tool/GrepTool/grep_tool.cpp
    tool/BashTool/bash_tool.cpp
    tool/PowerShellTool/powershell_tool.cpp
    tool/ShellTool/shell_detector.cpp
    tool/AgentTool/agent_tool.cpp
    tool/AskUser/AskUserTool.cpp
    tool/AskUser/ask_user_config.cpp
    tool/PlanMode/enter_plan_mode_tool.cpp
    tool/PlanMode/exit_plan_mode_v2_tool.cpp
    tool/Task/TaskOutputTool.cpp
    tool/Task/TaskStopTool.cpp
    tool/MCPTool/mcp_tool.cpp
    tool/WebFetchTool/web_fetch_tool.cpp
    tool/SkillTool/skill_tool.cpp

    # input: 头文件（显式列入以便 IDE 导航）
    input/parser.h
    input/processor.h
    input/types.h
)
