# 构建命令速查

| 目的 | 命令 |
|---|---|
| 配置（vcpkg toolchain） | `cmake --preset default` |
| 增量构建 | `cmake --build build --config Debug` |
| 只构建单测目标 | `cmake --build build --config Debug --target workx_unit_tests` |
| 全量测试 | `ctest --test-dir build -C Debug --output-on-failure` |
| 按名称筛选 | `ctest --test-dir build -C Debug -R "skill"` |
| 单测可执行文件筛选 | `build/bin/Debug/workx_unit_tests.exe "[skill]"` |
