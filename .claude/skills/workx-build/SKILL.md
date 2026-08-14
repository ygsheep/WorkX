---
name: workx-build
description: 构建 workx（Debug）并运行单元测试
aliases: wb
argument_hint: [target]
when_to_use: 修改了 C++ 源码后需要验证编译通过或运行测试时
---

# Workx Build & Test

在 workx 仓库中构建单元测试并运行（单元测试已**按模块拆分为 5 个独立目标**，见 refs/commands.md）：

1. `cmake --preset default`
2. 构建某个模块测试：`cmake --build build --config Release --target core_unit_tests -j 8`（可用 `tui_unit_tests` / `agent_unit_tests` / `island_unit_tests` / `app_unit_tests` 替换）
3. 运行测试：`ctest --test-dir build -C Release -j 8 --output-on-failure`

## 语法高亮（Tree-sitter）

代码块语法高亮基于 Tree-sitter，支持 **30 种主流语言**（为控制体积裁剪；grammar 静态链接进 exe，体积主要来自解析状态表）。语言清单维护在 `scripts/gen_ts_grammars.py` 的 `GRAMMARS`，注册代码（`cmake/ts_grammars.cmake` + `src/tui/render/*.inc`）由 `scripts/gen_ts_grammars.py` 自动生成；`scripts/fetch_ts_grammars.py` 可从官方 wiki 自动抓取新语言。详细命令见 refs/commands.md。

> **configure 提示**：configure 期会从 GitHub 拉取全部 grammar 仓库（需网络）。首次 configure 因 commit hash 走全量克隆可能耗时数十分钟，仓库缓存后再次 configure 约 2 分钟。若 `workx` 目标链接报 scanner 符号冲突或某 grammar 无法编译，先确认 `gen_ts_grammars.py` 已重新生成注册代码。

常用过滤器（ctest）：

- 并行跑全部测试：`ctest --test-dir build -C Release -j 8`
- 快速回归（跳过 [slow] 慢测试）：`ctest --test-dir build -C Release -LE slow -j 8`
- 只跑慢测试（验证超时/并发类逻辑）：`ctest --test-dir build -C Release -L slow -j 8`
- 按功能标签过滤：`ctest --test-dir build -C Release -L skill -j 8`
- 按名称筛选：`ctest --test-dir build -C Release -R skill -j 8`
- 单测可执行文件筛选：`build/bin/Release/core_unit_tests.exe "[skill]"`

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
