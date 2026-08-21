# src/CMakeLists.txt
# 分层库目标：workx_core（底层）→ workx_agent（Agent Harness，可安装）→ workx_island
# workx 可执行文件 = src/tui/main.cpp + FTXUI 渲染层 + workx_agent

# ============================================================
# 引入各分层（各自定义库目标）
# ============================================================
add_subdirectory(core)
add_subdirectory(agent)
add_subdirectory(island)
add_subdirectory(tui)

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

# ============================================================
# 安装与包导出（M2 库化：外部工程可 find_package(workx)）
# 参考 lib/liblogger 的 install/EXPORT 模板
# 仅安装 workx_core + workx_agent（Agent Harness）；workx (TUI) 为内部目标
# ============================================================
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# 公共头清单（M3 白名单；未列入即私有，不安装）
set(WORKX_PUBLIC_HEADERS
    core/events/agent_events.h
    core/export.h
    core/events/event_bus.h
    core/events/i_event_bus.h
    core/events/event_token.h
    core/events/events.h
    core/events/stream_events.h
    core/events/system_events.h
    core/config/config_manager.h
    core/config/i_config_manager.h
    core/task/task_manager.h
    core/task/thread_pool.h
    core/tool_kind.h
    core/utils/error.h
    core/utils/result.h
    core/utils/result_v2.h
    agent/core/chat_session.h
    agent/core/react_loop.h
    agent/core/react_observer.h
    agent/api/backend_types.h
    agent/api/chat_types.h
    agent/api/client.h
    agent/api/i_backend.h
    agent/api/i_backend_admin.h
    agent/api/i_completion_provider.h
    agent/api/i_stream_reader.h
    agent/api/retry.h
    agent/message/types.h
    agent/model/provider_type.h
    agent/compact/prefix_shape.h
    agent/compact/cache_aware_compactor.h
    agent/command/inclaude/command.h
    agent/command/inclaude/types.h
    agent/skill/inclaude/conditional.h
    agent/tool/registry.h
    agent/tool/executor.h
    agent/tool/context.h
    agent/tool/permission_ask.h
    agent/tool/types.h
    agent/tool/itool.h
    agent/tool/result.h
    agent/session/session_store.h
    agent/config/app_config.h
    agent/audit/audit_logger.h
    agent/tool/AskUser/ask_user_config.h
)

install(TARGETS workx_core workx_agent
    EXPORT workx_targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# 安装到 include/ 根并保持目录结构（install(FILES) 会拍平子目录，
# 必须按目录分组；源码以 agent/...、core/... 风格 include，
# 与 INSTALL_INTERFACE:include 对应，参考 liblogger 布局）
foreach(_hdr IN LISTS WORKX_PUBLIC_HEADERS)
    get_filename_component(_hdir "${_hdr}" DIRECTORY)
    install(FILES "${_hdr}" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/${_hdir}")
endforeach()

install(EXPORT workx_targets
    FILE workxTargets.cmake
    NAMESPACE workx::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/workx
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/workxConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/workxConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/workx
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/workxConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
    ARCH_INDEPENDENT
)

install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/workxConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/workxConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/workx
)
