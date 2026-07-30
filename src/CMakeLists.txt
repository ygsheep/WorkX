# src/CMakeLists.txt
# 创建 libworkx 静态库（聚合 core/agent/tui/app 四大模块源文件）
# 创建 workx 可执行文件（main.cpp + libworkx）

# ============================================================
# libworkx 静态库
# ============================================================
# 初始为空，由各子模块通过 target_sources() 追加源文件
add_library(libworkx STATIC)

# 公共 include 路径（src/ 作为根，所有模块以 src/ 为基准 #include）
target_include_directories(libworkx PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)

# 版本宏
target_compile_definitions(libworkx PUBLIC
    WORKX_VERSION="${PROJECT_VERSION}"
)

# 第三方依赖（PUBLIC：使用 libworkx 的目标也会继承）
if(nlohmann_json_FOUND)
    target_link_libraries(libworkx PUBLIC nlohmann_json::nlohmann_json)
    target_compile_definitions(libworkx PUBLIC WORKX_HAS_NLOHMANN_JSON)
endif()

target_link_libraries(libworkx PUBLIC
    CURL::libcurl
    logger
)
target_compile_definitions(libworkx PUBLIC WORKX_HAS_CURL)

# Tree-sitter（可选，由根 CMakeLists.txt 探测后传入变量）
if(WORKX_HAS_TREE_SITTER)
    target_link_libraries(libworkx PUBLIC ${WORKX_TS_RUNTIME_LIB})
    target_compile_definitions(libworkx PUBLIC WORKX_HAS_TREE_SITTER)
    if(WORKX_TS_GRAMMAR_LIBS)
        target_link_libraries(libworkx PRIVATE ${WORKX_TS_GRAMMAR_LIBS})
        target_compile_definitions(libworkx PUBLIC ${WORKX_TS_GRAMMAR_DEFS})
    endif()
endif()

# ============================================================
# 引入各业务模块（通过 target_sources 追加源文件到 libworkx）
# ============================================================
add_subdirectory(core)
add_subdirectory(agent)
add_subdirectory(tui)
add_subdirectory(app)

# ============================================================
# workx 可执行文件
# ============================================================
add_executable(workx app/main.cpp)
target_link_libraries(workx PRIVATE libworkx)

# ============================================================
# 应用图标：从 src/icon.png 生成各平台格式
#   Windows: src/icon.ico  → 通过 .rc 资源文件嵌入可执行文件
#   macOS:   src/icon.icns → 作为 MACOSX_BUNDLE 资源
#   Linux:   src/icon.png  → 拷贝到输出目录（供 .desktop 文件引用）
# 转换脚本：scripts/convert_icon.py（缺失 Pillow 时自动安装）
# ============================================================
set(WORKX_ICON_PNG "${CMAKE_CURRENT_SOURCE_DIR}/icon.png")
set(WORKX_ICON_ICO "${CMAKE_CURRENT_SOURCE_DIR}/icon.ico")
set(WORKX_ICON_ICNS "${CMAKE_CURRENT_SOURCE_DIR}/icon.icns")
set(WORKX_CONVERT_ICON_SCRIPT "${CMAKE_SOURCE_DIR}/scripts/convert_icon.py")

# 配置阶段调用脚本生成 ico/icns（增量构建，已是最新则跳过）
# 注意：脚本不自动安装 Pillow，缺失时仅提示，需开发者手动 pip install
find_program(WORKX_PYTHON_EXECUTABLE python python3)
if(WORKX_PYTHON_EXECUTABLE AND EXISTS "${WORKX_ICON_PNG}")
    execute_process(
        COMMAND "${WORKX_PYTHON_EXECUTABLE}" "${WORKX_CONVERT_ICON_SCRIPT}"
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE _icon_result
        OUTPUT_VARIABLE _icon_output
        ERROR_VARIABLE _icon_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_STRIP_TRAILING_WHITESPACE
    )
    if(_icon_output)
        message(STATUS "${_icon_output}")
    endif()
    if(_icon_result EQUAL 0)
        message(STATUS "Icon: conversion script completed")
    else()
        message(WARNING "Icon: conversion script failed: ${_icon_error}")
    endif()
elseif(NOT WORKX_PYTHON_EXECUTABLE)
    message(WARNING "Icon: Python not found, skip icon conversion")
endif()

# --- Windows：通过 RC 资源文件嵌入 ICO ---
if(WIN32 AND EXISTS "${WORKX_ICON_ICO}")
    set(WORKX_RC_FILE "${CMAKE_CURRENT_BINARY_DIR}/workx_icon.rc")
    # 使用数字 ID（1），Windows 资源管理器自动识别 exe 中的最低 ID ICON 资源
    file(WRITE "${WORKX_RC_FILE}"
        "1 ICON \"${WORKX_ICON_ICO}\"\n"
    )
    target_sources(workx PRIVATE "${WORKX_RC_FILE}")
    message(STATUS "Icon: Windows ICO embedded via RC file")
endif()

# --- macOS：设置 Bundle 图标 ---
if(APPLE)
    if(EXISTS "${WORKX_ICON_ICNS}")
        set_target_properties(workx PROPERTIES
            MACOSX_BUNDLE TRUE
            MACOSX_BUNDLE_ICON_FILE "icon.icns"
        )
        set_source_files_properties("${WORKX_ICON_ICNS}" PROPERTIES
            MACOSX_PACKAGE_LOCATION "Resources"
        )
        target_sources(workx PRIVATE "${WORKX_ICON_ICNS}")
        message(STATUS "Icon: macOS ICNS added to bundle Resources")
    else()
        message(WARNING "macOS build without icon: ${WORKX_ICON_ICNS} missing")
    endif()
endif()

# --- Linux：拷贝 PNG 到输出目录 ---
if(UNIX AND NOT APPLE AND EXISTS "${WORKX_ICON_PNG}")
    add_custom_command(TARGET workx POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${WORKX_ICON_PNG}"
            "$<TARGET_FILE_DIR:workx>/icon.png"
        COMMENT "Copying application icon: icon.png"
    )
    message(STATUS "Icon: Linux PNG copied to output dir")
endif()

# ============================================================
# vendor/ripgrep: 构建时拷贝到 workx 输出目录的 tools/ 子目录
# 二进制由 scripts/fetch-ripgrep.py 下载到 vendor/ripgrep/<platform>/
# 运行时由 ToolRegistry 从 <exe_dir>/tools/rg 查找
# 缺失时自动回退 C++ 标准库实现，不报错
#
# 注意：add_custom_command(TARGET workx POST_BUILD ...) 必须在 workx
#       目标所在的目录调用（CMake 限制）。源/目标路径变量由根
#       CMakeLists.txt 计算并通过普通变量传递（父作用域可见）。
# ============================================================
if(WORKX_HAS_VENDOR_RIPGREP)
    add_custom_command(TARGET workx POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:workx>/tools"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${WORKX_RG_SOURCE}"
            "$<TARGET_FILE_DIR:workx>/tools/${WORKX_RG_DEST_NAME}"
        COMMENT "Bundling ripgrep: ${WORKX_RG_DEST_NAME}"
    )
    target_compile_definitions(workx PRIVATE WORKX_HAS_BUNDLED_RIPGREP=1)
endif()
