# Workx TUI chat client build configuration
# @date 2026-07

cmake_minimum_required(VERSION 3.21)
project(workx VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
if(MSVC)
    add_compile_options(/W4 /utf-8 /Zc:preprocessor)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(nlohmann_json CONFIG QUIET)

find_package(CURL CONFIG REQUIRED)

# ---- Tree-sitter (语法高亮, 可选) ----
# 策略: tree-sitter runtime 和所有 grammar 都用 FetchContent 拉取, 不依赖 vcpkg
#       这样 vcpkg.json 不需要新增端口, 也不会因 vcpkg 缺少 tree-sitter port 导致 manifest 失败
option(WORKX_WITH_TREE_SITTER "Enable tree-sitter based syntax highlighting" ON)
option(WORKX_FETCH_GRAMMARS  "Fetch tree-sitter runtime + grammars via FetchContent (requires network at configure time)" ON)

set(WORKX_TS_RUNTIME_LIB "")
set(WORKX_TS_GRAMMAR_LIBS "")
set(WORKX_TS_GRAMMAR_DEFS "")

# 优先用 vcpkg 提供的 tree-sitter runtime (若已装), 否则 FetchContent 拉取
if(WORKX_WITH_TREE_SITTER)
    find_package(unofficial-tree-sitter CONFIG QUIET)
    if(unofficial-tree-sitter_FOUND)
        set(WORKX_TS_RUNTIME_LIB "unofficial::tree-sitter::tree-sitter")
        set(WORKX_HAS_TREE_SITTER TRUE)
        message(STATUS "Tree-sitter runtime: found via vcpkg (unofficial::tree-sitter)")
    else()
        find_package(tree-sitter CONFIG QUIET)
        if(tree-sitter_FOUND)
            set(WORKX_TS_RUNTIME_LIB "tree-sitter::tree-sitter")
            set(WORKX_HAS_TREE_SITTER TRUE)
            message(STATUS "Tree-sitter runtime: found via vcpkg (tree-sitter::tree-sitter)")
        endif()
    endif()
endif()

# vcpkg 没有 tree-sitter 时, 用 FetchContent 拉取 tree-sitter 主仓库并编译 lib/src/lib.c
# lib.c 是汇总文件, include 了所有 runtime .c 文件, 编译它一个就够
function(workx_fetch_ts_runtime)
    if(NOT WORKX_WITH_TREE_SITTER)
        return()
    endif()
    if(WORKX_HAS_TREE_SITTER)
        return()  # vcpkg 已提供, 无需再 fetch
    endif()
    if(NOT WORKX_FETCH_GRAMMARS)
        message(STATUS "Tree-sitter: not available and WORKX_FETCH_GRAMMARS=OFF, highlighting disabled")
        return()
    endif()

    include(FetchContent)
    FetchContent_Declare("ts_runtime"
        GIT_REPOSITORY "https://github.com/tree-sitter/tree-sitter.git"
        GIT_TAG        "v0.25.10"
        GIT_SHALLOW    TRUE
        SOURCE_SUBDIR  "cmake_subdir_disabled"  # 跳过 tree-sitter 自带 CMakeLists (会要 tree-sitter-cli)
    )
    FetchContent_MakeAvailable("ts_runtime")
    FetchContent_GetProperties("ts_runtime" SOURCE_DIR _runtime_src_dir)

    set(_lib_src "${_runtime_src_dir}/lib/src/lib.c")
    if(NOT EXISTS "${_lib_src}")
        message(WARNING "Tree-sitter runtime: lib/src/lib.c not found at ${_runtime_src_dir}, highlighting disabled")
        return()
    endif()

    add_library("ts_runtime" STATIC "${_lib_src}")
    # tree-sitter 内部源码用 #include "tree_sitter/parser.h" 等, 需要 lib/src 和 lib/include
    target_include_directories("ts_runtime"
        PUBLIC
            "${_runtime_src_dir}/lib/include"
            "${_runtime_src_dir}/lib/src"
    )
    # MSVC 静态库 + CRT 一致性
    if(MSVC)
        target_compile_options("ts_runtime" PRIVATE /utf-8)
    endif()

    set(WORKX_TS_RUNTIME_LIB "ts_runtime" PARENT_SCOPE)
    set(WORKX_HAS_TREE_SITTER TRUE PARENT_SCOPE)
    message(STATUS "Tree-sitter runtime: fetched via FetchContent (v0.25.10)")
endfunction()
workx_fetch_ts_runtime()

# 通过 FetchContent 拉取 grammar 仓库 (各仓库已 commit 预生成的 src/parser.c, 无需 tree-sitter-cli)
# 每个 grammar 编译为独立静态库; 收集到 WORKX_TS_GRAMMAR_LIBS/DEFS, 在 libworkx 创建后统一链接
function(workx_fetch_ts_grammar name git_repo git_tag)
    if(NOT WORKX_HAS_TREE_SITTER)
        return()
    endif()
    if(NOT WORKX_FETCH_GRAMMARS)
        return()
    endif()
    string(TOUPPER "${name}" _upper)

    include(FetchContent)
    FetchContent_Declare("ts_${name}"
        GIT_REPOSITORY "${git_repo}"
        GIT_TAG "${git_tag}"
        GIT_SHALLOW TRUE
        SOURCE_SUBDIR "cmake_subdir_disabled"  # 跳过 grammar 自带 CMakeLists (会要 tree-sitter-cli)
    )
    FetchContent_MakeAvailable("ts_${name}")
    FetchContent_GetProperties("ts_${name}" SOURCE_DIR _grammar_src_dir)

    set(_src_dir "${_grammar_src_dir}/src")
    set(_srcs)
    if(EXISTS "${_src_dir}/parser.c")
        list(APPEND _srcs "${_src_dir}/parser.c")
    endif()
    # 部分 grammar (cpp/python/js/rust/...) 有外部 scanner, 扩展名可能是 .c 或 .cc
    if(EXISTS "${_src_dir}/scanner.c")
        list(APPEND _srcs "${_src_dir}/scanner.c")
    endif()
    if(EXISTS "${_src_dir}/scanner.cc")
        list(APPEND _srcs "${_src_dir}/scanner.cc")
    endif()

    if(NOT _srcs)
        message(WARNING "Tree-sitter grammar '${name}': parser.c not found at ${_src_dir}, skipped")
        return()
    endif()

    add_library("ts_${name}" STATIC ${_srcs})
    target_include_directories("ts_${name}" PUBLIC "${_src_dir}")
    target_link_libraries("ts_${name}" PUBLIC ${WORKX_TS_RUNTIME_LIB})
    if(MSVC)
        target_compile_options("ts_${name}" PRIVATE /utf-8)
    endif()

    # 累积到 parent scope, 等 libworkx 创建后再 target_link_libraries
    list(APPEND WORKX_TS_GRAMMAR_LIBS "ts_${name}")
    list(APPEND WORKX_TS_GRAMMAR_DEFS "WORKX_TS_${_upper}")
    set(WORKX_TS_GRAMMAR_LIBS "${WORKX_TS_GRAMMAR_LIBS}" PARENT_SCOPE)
    set(WORKX_TS_GRAMMAR_DEFS "${WORKX_TS_GRAMMAR_DEFS}" PARENT_SCOPE)
    message(STATUS "  + tree-sitter-${name}: ${git_tag}")
endfunction()

if(WORKX_HAS_TREE_SITTER AND WORKX_FETCH_GRAMMARS)
    message(STATUS "Fetching tree-sitter grammars:")
    # 标签可按需调整; grammar ABI 向后兼容 tree-sitter runtime 0.25+/0.26+
    workx_fetch_ts_grammar(c            "https://github.com/tree-sitter/tree-sitter-c.git"           "v0.24.2")
    workx_fetch_ts_grammar(cpp          "https://github.com/tree-sitter/tree-sitter-cpp.git"         "v0.23.4")
    workx_fetch_ts_grammar(cmake        "https://github.com/uyha/tree-sitter-cmake.git"              "v0.5.0")
    workx_fetch_ts_grammar(python       "https://github.com/tree-sitter/tree-sitter-python.git"      "v0.23.6")
    workx_fetch_ts_grammar(bash         "https://github.com/tree-sitter/tree-sitter-bash.git"        "v0.23.3")
    workx_fetch_ts_grammar(json         "https://github.com/tree-sitter/tree-sitter-json.git"        "v0.24.8")
    workx_fetch_ts_grammar(javascript   "https://github.com/tree-sitter/tree-sitter-javascript.git"  "v0.23.1")
    workx_fetch_ts_grammar(rust         "https://github.com/tree-sitter/tree-sitter-rust.git"        "v0.23.1")
    workx_fetch_ts_grammar(go           "https://github.com/tree-sitter/tree-sitter-go.git"          "v0.23.4")
endif()

set(WORKX_SOURCES
    src/core/task/task_manager.cpp
    src/agent/api/sse_parser.cpp
    src/core/config/config_manager.cpp
    src/tui/core/terminal.cpp
    src/tui/input/line_editor.cpp
    src/tui/core/color_scheme.cpp
    src/tui/render/chat_renderer.cpp
    src/tui/render/output_formatter.cpp
    src/tui/render/streaming_buffer.cpp
    src/tui/widgets/status_bar.cpp
    src/tui/widgets/command_panel.cpp
    src/tui/widgets/file_search_panel.cpp
    src/tui/widgets/select_panel.cpp
    src/tui/widgets/bottom_bar_manager.cpp
    src/tui/setup/setup_wizard.cpp
    src/tui/core/screen.cpp
    src/tui/core/display_buffer.cpp
    src/tui/utils/utf8_utils.cpp
    src/tui/render/markdown_renderer.cpp
    src/tui/render/spinner.cpp
    src/tui/render/syntax_highlighter.cpp
    src/tui/input/history.cpp
    src/agent/core/chat_session.cpp
    src/agent/api/backend_factory.cpp
    src/agent/api/client.cpp
    src/agent/api/remote/remote_backend.cpp
    src/agent/api/remote/sse_stream_reader.cpp
    src/agent/api/remote/http_client.cpp
    src/agent/model/provider_preset.cpp
    src/agent/api/provider/openai_adapter.cpp
    src/agent/api/provider/anthropic_adapter.cpp
    src/agent/command/source/registry.cpp
    src/agent/command/source/executor.cpp
    src/agent/tool/encoding.cpp
    src/agent/tool/FileReadTool/file_read_tool.cpp
    src/agent/tool/FileEditTool/file_edit_tool.cpp
    src/agent/tool/FileWriteTool/file_write_tool.cpp
    src/agent/tool/FileWriteTool/diff.cpp
    src/agent/tool/FileReadState/file_read_state.cpp
    src/agent/tool/GlobTool/glob_tool.cpp
    src/agent/tool/GrepTool/grep_tool.cpp
    src/agent/tool/BashTool/bash_tool.cpp
    src/agent/tool/AgentTool/agent_tool.cpp
    src/agent/tool/MCPTool/mcp_tool.cpp
    src/agent/tool/WebFetchTool/web_fetch_tool.cpp
    src/app/command/builtin_commands.cpp
    src/app/config/app_config.cpp
    src/app/config/cli_args.cpp
    src/app/ui/model_selector.cpp
    src/app/ui/path_completer.cpp
    src/app/ui/file_index.cpp
    src/agent/input/parser.h
    src/agent/input/processor.h
    src/agent/input/types.h
)

# Add platform-specific source
if(WIN32)
    list(APPEND WORKX_SOURCES src/tui/core/platform/platform_win32.cpp)
else()
    list(APPEND WORKX_SOURCES src/tui/core/platform/platform_posix.cpp)
endif()

add_library(libworkx STATIC ${WORKX_SOURCES})

# end modification: platform-specific sources handled conditionally
target_include_directories(libworkx PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
    $<INSTALL_INTERFACE:include>
)

target_compile_definitions(libworkx PUBLIC
    WORKX_VERSION="${PROJECT_VERSION}"
)

if(nlohmann_json_FOUND)
    target_link_libraries(libworkx PUBLIC nlohmann_json::nlohmann_json)
    target_compile_definitions(libworkx PUBLIC WORKX_HAS_NLOHMANN_JSON)
endif()

target_link_libraries(libworkx PUBLIC
    CURL::libcurl
    logger
 )
target_compile_definitions(libworkx PUBLIC WORKX_HAS_CURL)

if(WORKX_HAS_TREE_SITTER)
    target_link_libraries(libworkx PUBLIC ${WORKX_TS_RUNTIME_LIB})
    target_compile_definitions(libworkx PUBLIC WORKX_HAS_TREE_SITTER)
    if(WORKX_TS_GRAMMAR_LIBS)
        target_link_libraries(libworkx PRIVATE ${WORKX_TS_GRAMMAR_LIBS})
        target_compile_definitions(libworkx PUBLIC ${WORKX_TS_GRAMMAR_DEFS})
    endif()
endif()

add_subdirectory(lib/liblogger)

include_directories(${CMAKE_SOURCE_DIR}/lib)

add_executable(workx src/app/main.cpp)
target_link_libraries(workx PRIVATE libworkx)

option(WORKX_BUILD_TESTS "Build tests" ON)
if(WORKX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

option(WORKX_BUILD_EXAMPLES "Build examples" ON)
if(WORKX_BUILD_EXAMPLES)
    add_subdirectory(example/example_markdown)
    add_subdirectory(example/example_code_highlight)
endif()