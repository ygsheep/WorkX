# DearTs Logger - Modern C++20 Logging Library
# Independent third-party library for DearTs project

cmake_minimum_required(VERSION 3.20)
project(logger VERSION 1.0.0 LANGUAGES CXX)

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 输出目录配置
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# 包含目录
include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# 编译选项
if(MSVC)
    # MSVC 特定选项
    add_compile_options(/W4)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
    # 确保 Windows.h 不定义 min/max 宏
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    # GCC/Clang 选项
    add_compile_options(-Wall -Wextra -Wpedantic)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-g -O0)
    else()
        add_compile_options(-O3)
    endif()
endif()

# 源文件
set(LOGGER_SOURCES
    logger.cpp
)

# 头文件
set(LOGGER_HEADERS
    logger.h
)

# 静态库
add_library(logger STATIC ${LOGGER_SOURCES})

# 设置目标属性
target_compile_features(logger PUBLIC cxx_std_20)

# 导出包含目录
# BUILD_INTERFACE 指向 lib/（父目录）：workx 以 <liblogger/logger.h> 风格 include，
# 该风格在构建树与安装树（include/liblogger/logger.h）保持一致
target_include_directories(logger
    PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
    $<INSTALL_INTERFACE:include>
)

# 平台特定链接
if(WIN32)
    target_link_libraries(logger
        PUBLIC
        # Windows 库
    )
elseif(APPLE)
    target_link_libraries(logger
        PUBLIC
        # macOS 框架
    )
else()
    target_link_libraries(logger
        PUBLIC
        # Linux 库
        pthread
    )
endif()

# 安装配置
include(GNUInstallDirs)

install(TARGETS logger
    EXPORT logger_targets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# 安装到 include/liblogger/，与 <liblogger/logger.h> include 风格对应
install(FILES ${LOGGER_HEADERS}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/liblogger
)

# 导出配置
# 无 NAMESPACE：目标在安装树中仍名为 logger（与构建树一致），
# workx 导出文件中的 "logger" 引用在 find_dependency(logger) 后才能解析
install(EXPORT logger_targets
    FILE logger_targets.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/logger)


# 库配置输出
message(STATUS "")
message(STATUS "=== DearTs Logger Configuration ===")
message(STATUS "Version: ${PROJECT_VERSION}")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ Standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "Platform: ${CMAKE_SYSTEM_NAME}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "")
message(STATUS "Features:")
message(STATUS "  - Static Library: YES")
message(STATUS "  - Thread Safe: YES")
message(STATUS "  - Async File I/O: YES")
message(STATUS "  - Duplicate Filter: YES")
message(STATUS "  - Cross Platform: YES")
message(STATUS "  - std::format Support: YES")  # 新增
if(WIN32)
    message(STATUS "  - Windows Support: YES")
elseif(APPLE)
    message(STATUS "  - macOS Support: YES")
else()
    message(STATUS "  - Linux Support: YES")
endif()
message(STATUS "")

# 创建包配置文件
include(CMakePackageConfigHelpers)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/loggerConfig.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/loggerConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/logger
)

write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/loggerConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
    ARCH_INDEPENDENT
)

# 安装配置文件
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/loggerConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/loggerConfigVersion.cmake"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/logger
)