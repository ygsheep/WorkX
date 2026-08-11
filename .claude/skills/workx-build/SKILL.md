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

## 版本管理

版本号**单一事实源**在 `cmake/version.cmake`（SemVer，0.x 阶段：MINOR=不兼容变更、PATCH=兼容修复）。**禁止手改版本号**，一律用脚本：

| 目的 | 命令 |
|---|---|
| 查看当前版本 | `python scripts/bump_version.py show` |
| 自动升版（按文件级版本聚合） | `python scripts/bump_version.py auto` |
| 升 patch（bugfix） | `python scripts/bump_version.py patch` |
| 升 minor（不兼容变更） | `python scripts/bump_version.py minor` |
| 升 major（1.0 正式发布） | `python scripts/bump_version.py major` |

脚本会自动同步 `vcpkg.json`、`tests/consumer/vcpkg.json`、`flake.nix`、`nix/workx.nix`。cmake configure 期会校验这些文件与 `PROJECT_VERSION` 一致，不一致直接 `FATAL_ERROR`（所以升版后必须重新 configure）。

### 文件级版本号（@version，src/core + src/agent）

两层版本体系：文件头 `@version` 随改随升（细粒度），聚合后驱动项目版本。

| 目的 | 命令 |
|---|---|
| 扫描改动文件并更新 @version（小改+0.0.1） | `python scripts/version_files.py` |
| 标记大改文件（+0.1） | `python scripts/version_files.py --minor <file> [--minor <file> ...]` |
| 只预览不写入 | `python scripts/version_files.py --dry-run` |
| 发布前看聚合统计（相对最近 v-tag） | `python scripts/version_files.py --stat` |

聚合规则（`bump_version.py auto` 自动执行）：任一文件大改(+0.1) → 项目升 **minor**；小改/新增文件数 ≥ 10 → 项目升 **patch**；都不满足 → 不升。范围仅 `src/core`、`src/agent`（对外发布库模块）。

### 二进制内嵌版本宏

`src/core` / `src/agent` 各 PUBLIC 定义三个宏：
- `WORKX_VERSION`：发布版本（`PROJECT_VERSION`，如 `0.2.0`），用于 `find_package` 兼容判断
- `WORKX_BUILD_INFO`：构建版本 `<版本>+<git describe>`（如 `0.2.0+896e5be`、`0.2.0+0.2.0-3-gabc1234`、`...-dirty`），欢迎界面与 island 握手展示，用于产物溯源
- `WORKX_FILE_VERSION`：文件级 @version 聚合（`m<小改数>M<大改数>`，如 `m3M1`，相对最近 v-tag；无 tag 时为空），随 `WORKX_BUILD_INFO` 一并展示

发布流程：改代码（`version_files.py` 升文件版本）→ `bump_version.py auto|minor|patch` → 重新 configure + 构建 + 测试 → 打 tag `git tag v<版本> && git push origin v<版本>`。

构建细节见 refs/commands.md。
