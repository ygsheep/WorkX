# Workx TUI chat client build configuration
# @date 2026-07

cmake_minimum_required(VERSION 3.21)

# workx 仓库根（外部工程 add_subdirectory 时 CMAKE_SOURCE_DIR 指向外部，此处仍须指向 workx 根）
set(WORKX_ROOT "${CMAKE_CURRENT_LIST_DIR}")

# 版本单一事实源（含多文件一致性校验与构建版本派生，见 cmake/version.cmake）
include(cmake/version.cmake)

project(workx VERSION ${WORKX_VERSION_MAJOR}.${WORKX_VERSION_MINOR}.${WORKX_VERSION_PATCH} LANGUAGES C CXX)

# ---- 版本一致性校验 + 构建版本号派生（需在 project() 之后）----
workx_check_version_file("vcpkg.json"                 "\"version-string\": \"${PROJECT_VERSION}\"")
workx_check_version_file("tests/consumer/vcpkg.json"  "\"version-string\": \"${PROJECT_VERSION}\"")
workx_check_version_file("flake.nix"                  "version = \"${PROJECT_VERSION}\";")
workx_check_version_file("nix/workx.nix"              "version = \"${PROJECT_VERSION}\";")
workx_derive_build_info()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
if(MSVC)
    add_compile_options(/W4 /utf-8 /Zc:preprocessor /FS)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    # 提升未初始化变量警告为错误
    add_compile_options(/we4700 /we4701 /we4702 /we4703)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wuninitialized -Wmaybe-uninitialized)
    add_compile_options(-Werror=uninitialized -Werror=maybe-uninitialized)
endif()

# nlohmann_json / CURL 是硬依赖：公共头无条件 include <nlohmann/json.hpp>
# （见 core/config/i_config_manager.h 等 17 处），src/agent 无条件编译
# http_client.cpp。REQUIRED 让依赖缺失在 configure 期直接失败（错误可定位），
# 而不是静默跳过链接后到编译期才爆 C1083。
# 注意 add_subdirectory 消费模式：vcpkg manifest 只对顶层工程生效，消费方
# 须自行提供这两个包（manifest 声明或已安装实例），workx 无法代为获取。
find_package(nlohmann_json CONFIG REQUIRED)

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
# 每个 grammar 编译为独立静态库; 收集到 WORKX_TS_GRAMMAR_LIBS/DEFS, 由 workx_tui 统一链接
function(workx_fetch_ts_grammar name git_repo git_tag)
    # 第 4 参数：grammar 源码子目录（默认 "src"；tree-sitter-typescript 双子目录用 "typescript/src"/"tsx/src"）
    if(ARGC GREATER 3)
        set(_grammar_subdir "${ARGV3}")
    else()
        set(_grammar_subdir "src")
    endif()
    if(NOT WORKX_HAS_TREE_SITTER)
        return()
    endif()
    if(NOT WORKX_FETCH_GRAMMARS)
        return()
    endif()
    # 宏名容错: c-sharp -> C_SHARP (target 名保留连字符, 仅宏名替换)
    string(REPLACE "-" "_" _upper "${name}")
    string(TOUPPER "${_upper}" _upper)

    include(FetchContent)
    # GIT_TAG 为 40 位 commit hash 时禁用 shallow clone:
    # 若该 commit 不是分支 tip 可达, git shallow clone 会长时间卡住/失败。
    # 具名 tag (如 v0.24.2) 保持 shallow, 加速 fetch。
    # 注意: CMake if(MATCHES) 正则不支持 {n} 计数量词, 需 LENGTH + 字符类组合校验
    string(LENGTH "${git_tag}" _tag_len)
    if(_tag_len EQUAL 40 AND git_tag MATCHES "^[0-9a-f]+$")
        set(_ts_shallow FALSE)
    else()
        set(_ts_shallow TRUE)
    endif()
    FetchContent_Declare("ts_${name}"
        GIT_REPOSITORY "${git_repo}"
        GIT_TAG "${git_tag}"
        GIT_SHALLOW ${_ts_shallow}
        GIT_SUBMODULES ""  # CMP0097=NEW 时, 空串表示不初始化任何 submodule
        SOURCE_SUBDIR "cmake_subdir_disabled"  # 跳过 grammar 自带 CMakeLists (会要 tree-sitter-cli)
    )
    FetchContent_MakeAvailable("ts_${name}")
    FetchContent_GetProperties("ts_${name}" SOURCE_DIR _grammar_src_dir)

    set(_src_dir "${_grammar_src_dir}/${_grammar_subdir}")
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

    # 部分第三方 grammar 的 scanner.c 定义了非 static 的通用函数
    # serialize/deserialize/scan (未加 static), 链接时会与其它 grammar 冲突。
    # 用宏把它们重命名为 <grammar>_<func>, 保证符号唯一。
    # 已知受影响的 grammar 列在 _SCANNER_GENERIC_FUNCS 中 (org/svelte 等)。
    set(_SCANNER_GENERIC_FUNCS serialize deserialize scan)
    set(_scanner_has_generic FALSE)
    if(EXISTS "${_src_dir}/scanner.c")
        file(READ "${_src_dir}/scanner.c" _scanner_src)
        foreach(_fn ${_SCANNER_GENERIC_FUNCS})
            # 非 static 定义: 行首(可选空白)直接是返回类型 + 函数名, 无 "static " 前缀
            if(_scanner_src MATCHES "(^|[;\n][ \t]*)${_fn}[ \t]*\\("
               AND NOT _scanner_src MATCHES "(^|[;\n][ \t]*)static[ \t]+[^;]*${_fn}[ \t]*\\(")
                set(_scanner_has_generic TRUE)
            endif()
        endforeach()
        if(_scanner_has_generic)
            string(REPLACE "-" "_" _name_underscore "${name}")
            target_compile_definitions("ts_${name}" PRIVATE
                serialize=${_name_underscore}_serialize
                deserialize=${_name_underscore}_deserialize
                scan=${_name_underscore}_scan
            )
            message(STATUS "  * tree-sitter-${name}: scanner generic funcs renamed (${_name_underscore}_*)")
        endif()
    endif()

    # 累积到 parent scope, 等 workx_tui 创建后再 target_link_libraries
    list(APPEND WORKX_TS_GRAMMAR_LIBS "ts_${name}")
    list(APPEND WORKX_TS_GRAMMAR_DEFS "WORKX_TS_${_upper}")
    set(WORKX_TS_GRAMMAR_LIBS "${WORKX_TS_GRAMMAR_LIBS}" PARENT_SCOPE)
    set(WORKX_TS_GRAMMAR_DEFS "${WORKX_TS_GRAMMAR_DEFS}" PARENT_SCOPE)
    message(STATUS "  + tree-sitter-${name}: ${git_tag}")
endfunction()

# grammar 清单由 scripts/gen_ts_grammars.py 生成 (cmake/ts_grammars.cmake)
# 新增语言: 在 scripts/gen_ts_grammars.py 的 GRAMMARS 列表加一行后重跑该脚本即可
include(cmake/ts_grammars.cmake)

# ---- liblogger (lib/liblogger/CMakeLists.txt 提供 logger target) ----
add_subdirectory(lib/liblogger)

# 让 workx 相关目标能 #include <liblogger/...>
# 用 CMAKE_CURRENT_LIST_DIR 而非 CMAKE_SOURCE_DIR：workx 被外部工程
# add_subdirectory 纳入时，CMAKE_SOURCE_DIR 指向外部工程根，此处仍须指向 workx 根
include_directories(${CMAKE_CURRENT_LIST_DIR}/lib)

# ============================================================
# vendor/ripgrep: 构建时拷贝到 workx 输出目录的 tools/ 子目录
# 二进制由 scripts/fetch-ripgrep.py 下载到 vendor/ripgrep/<platform>/
# 运行时由 ToolRegistry 从 <exe_dir>/tools/rg 查找
# 缺失时自动回退 C++ 标准库实现，不报错
#
# 注意：add_custom_command(TARGET workx POST_BUILD ...) 必须在 workx
#       目标所在的目录（src/CMakeLists.txt）调用（CMake 限制）。
#       这里在 add_subdirectory(src) 之前计算源/目标路径变量，
#       通过变量作用域传递给子目录。
# ============================================================
set(WORKX_VENDOR_RIPGREP_DIR ${CMAKE_SOURCE_DIR}/vendor/ripgrep)

if(WIN32)
    set(WORKX_RG_SOURCE ${WORKX_VENDOR_RIPGREP_DIR}/windows/rg.exe)
    set(WORKX_RG_DEST_NAME rg.exe)
elseif(APPLE)
    set(WORKX_RG_SOURCE ${WORKX_VENDOR_RIPGREP_DIR}/macos/rg)
    set(WORKX_RG_DEST_NAME rg)
else()
    set(WORKX_RG_SOURCE ${WORKX_VENDOR_RIPGREP_DIR}/linux/rg)
    set(WORKX_RG_DEST_NAME rg)
endif()

if(EXISTS "${WORKX_RG_SOURCE}")
    set(WORKX_HAS_VENDOR_RIPGREP TRUE)
    message(STATUS "ripgrep: vendor binary found at ${WORKX_RG_SOURCE}")
else()
    set(WORKX_HAS_VENDOR_RIPGREP FALSE)
    message(STATUS "ripgrep: vendor binary not found at ${WORKX_RG_SOURCE}, fallback to C++ implementation")
endif()

# ============================================================
# vendor/jq: 单文件 jq 拷贝到输出目录的 tools/ 子目录
# 二进制由 scripts/fetch-jq.py 下载到 vendor/jq/<platform>/
# 运行时由 ToolRegistry 的 resolve_jq() 从 <exe_dir>/tools/jq 查找
# 缺失时自动回退 PATH 中的 jq，仍无则不启用，不报错
# ============================================================
set(WORKX_VENDOR_JQ_DIR ${CMAKE_SOURCE_DIR}/vendor/jq)

if(WIN32)
    set(WORKX_JQ_SOURCE ${WORKX_VENDOR_JQ_DIR}/windows/jq.exe)
    set(WORKX_JQ_DEST_NAME jq.exe)
elseif(APPLE)
    set(WORKX_JQ_SOURCE ${WORKX_VENDOR_JQ_DIR}/macos/jq)
    set(WORKX_JQ_DEST_NAME jq)
else()
    set(WORKX_JQ_SOURCE ${WORKX_VENDOR_JQ_DIR}/linux/jq)
    set(WORKX_JQ_DEST_NAME jq)
endif()

if(EXISTS "${WORKX_JQ_SOURCE}")
    set(WORKX_HAS_VENDOR_JQ TRUE)
    message(STATUS "jq: vendor binary found at ${WORKX_JQ_SOURCE}")
else()
    set(WORKX_HAS_VENDOR_JQ FALSE)
    message(STATUS "jq: vendor binary not found at ${WORKX_JQ_SOURCE}, fallback to PATH (or disabled)")
endif()

# ============================================================
# 业务源码（src/ 下按 core/agent/tui/app 模块拆分，详见各 CMakeLists.txt）
# ============================================================
add_subdirectory(src)

option(WORKX_BUILD_TESTS "Build tests" ON)
if(WORKX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

option(WORKX_BUILD_CONSUMER "Build external-consumer smoke (tests/consumer, add_subdirectory mode)" OFF)
if(WORKX_BUILD_CONSUMER)
    set(WORKX_CONSUMER_ADD_SUBDIRECTORY ON)
    set(WORKX_SOURCE_DIR ${CMAKE_SOURCE_DIR})
    add_subdirectory(tests/consumer)
endif()

option(WORKX_BUILD_EXAMPLES "Build examples" ON)
if(WORKX_BUILD_EXAMPLES)
    add_subdirectory(example/example_dynamic_island)
endif()
