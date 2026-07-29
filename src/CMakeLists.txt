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
