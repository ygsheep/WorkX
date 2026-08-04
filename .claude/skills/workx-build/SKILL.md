---
name: workx-build
description: 构建 workx（Debug）并运行单元测试
aliases: wb
argument_hint: [target]
when_to_use: 修改了 C++ 源码后需要验证编译通过或运行测试时
---

# Workx Build & Test

在 workx 仓库中构建 Debug 配置并运行单元测试：

1. `cmake --preset default`
2. `cmake --build build --config Debug --target workx_unit_tests`
3. `ctest --test-dir build -C Debug --output-on-failure`

常用过滤器（ctest 或 workx_unit_tests.exe）：

- 只跑 skill 相关：`--test-dir build -C Debug -R skill`
- 单测筛选标签：`build/bin/Debug/workx_unit_tests.exe "[skill]"`

构建细节见 refs/commands.md。
