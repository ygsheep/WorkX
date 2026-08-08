# 消息灵动岛原型（Dear ImGui + Win32 透明置顶窗口 + D3D11）
# 桥接 workx EventBus：订阅 agent 宿主无关事件 → 动画通知岛
# 依赖: imgui[dx11-binding,win32-binding]（vcpkg）、系统 d3d11/dxgi/d3dcompiler
# 仅限 Windows：dx11/win32 binding 在 Linux 上无对应后端；vcpkg.json 里
# imgui 也声明了 platform: windows，Linux/NixOS 构建直接跳过本示例。
if(NOT WIN32)
    message(STATUS "example_dynamic_island: skipped (Windows only)")
    return()
endif()

find_package(imgui CONFIG REQUIRED)

add_executable(example_dynamic_island
    main.cpp
    dynamic_island.cpp
    event_bridge.cpp
    imgui_backends/imgui_impl_win32.cpp
    imgui_backends/imgui_impl_dx11.cpp
)

target_include_directories(example_dynamic_island PRIVATE
    imgui_backends
)

target_link_libraries(example_dynamic_island PRIVATE
    workx_core
    imgui::imgui
    d3d11
    dxgi
    d3dcompiler
)

if(MSVC)
    target_compile_features(example_dynamic_island PRIVATE cxx_std_20)
endif()
