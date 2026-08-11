# 构建命令速查

| 目的 | 命令 |
|---|---|
| 配置（vcpkg toolchain） | `cmake --preset default` |
| 增量构建 | `cmake --build build --config Debug` |
| 只构建单测目标 | `cmake --build build --config Debug --target workx_unit_tests` |
| 全量测试 | `ctest --test-dir build -C Debug --output-on-failure` |
| 按名称筛选 | `ctest --test-dir build -C Debug -R "skill"` |
| 单测可执行文件筛选 | `build/bin/Debug/workx_unit_tests.exe "[skill]"` |

## 版本命令

| 目的 | 命令 |
|---|---|
| 查看当前版本 | `python scripts/bump_version.py show` |
| 自动升版（按文件级版本聚合） | `python scripts/bump_version.py auto` |
| 升 patch（bugfix） | `python scripts/bump_version.py patch` |
| 升 minor（不兼容变更） | `python scripts/bump_version.py minor` |
| 升 major（1.0 正式发布） | `python scripts/bump_version.py major` |
| 发布打 tag | `git tag v<版本> && git push origin v<版本>` |

升版后需重新 configure（`cmake --preset default`），configure 期会校验 `vcpkg.json` / `flake.nix` / `nix/workx.nix` 与版本一致。

## 文件级版本号命令（src/core、src/agent @version）

| 目的 | 命令 |
|---|---|
| 更新改动文件的 @version（小改+0.0.1） | `python scripts/version_files.py` |
| 标记大改文件（+0.1） | `python scripts/version_files.py --minor src/core/xxx.h` |
| 只预览不写入 | `python scripts/version_files.py --dry-run` |
| 发布前看聚合统计（相对最近 v-tag） | `python scripts/version_files.py --stat` |

聚合规则：任一文件大改(+0.1) → minor；小改/新增数 ≥ 10 → patch。
