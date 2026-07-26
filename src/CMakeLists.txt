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
