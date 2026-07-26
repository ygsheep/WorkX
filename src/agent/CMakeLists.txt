# Agent 层：api / command / compact / core / input / message / model / tool

target_sources(libworkx PRIVATE
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

    # compact
    compact/token_count.cpp

    # core
    core/chat_session.cpp
    core/react_loop.cpp

    # model
    model/provider_preset.cpp
    model/config.cpp
    model/context_resolver.cpp

    # tool: 通用工具
    tool/encoding.cpp
    tool/path_matcher.cpp
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
    tool/AgentTool/agent_tool.cpp
    tool/MCPTool/mcp_tool.cpp
    tool/WebFetchTool/web_fetch_tool.cpp

    # input: 头文件（显式列入以便 IDE 导航）
    input/parser.h
    input/processor.h
    input/types.h
)
