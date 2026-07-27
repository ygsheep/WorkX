# mydev:cmake — CMake 配置

## 基础配置

```cmake
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4 /WX /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()
```

## 习惯

- C++20 为基准
- 编译器警告作为错误（/WX / -Werror）
- MSVC 使用 /utf-8 源码编码
- vcpkg 工具链管理第三方依赖
