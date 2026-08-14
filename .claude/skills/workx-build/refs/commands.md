# 构建命令速查

## 语法高亮（Tree-sitter）

代码块语法高亮基于 Tree-sitter，支持 **30 种主流语言**（为控制 workx.exe 体积裁剪，grammar 静态链接，体积来自解析状态表 `.rdata`；加回更多语言体积会增大）。语言清单与注册代码由脚本自动生成，新增语言不需要手改 CMake/C++。

| 目的 | 命令 |
|---|---|
| 从 wiki 自动获取可用 grammar 并解析 HEAD commit | `python scripts/fetch_ts_grammars.py --list` |
| 解析后打印清单行（不写文件） | `python scripts/fetch_ts_grammars.py --resolve` |
| 解析并直接更新 GRAMMARS 清单 | `python scripts/fetch_ts_grammars.py --update` |
| 忽略白名单处理全部候选 | `python scripts/fetch_ts_grammars.py --all` |
| 重新生成注册代码（.cmake + .inc） | `python scripts/gen_ts_grammars.py` |
| 只打印语言清单（不写文件） | `python scripts/gen_ts_grammars.py --list` |
| 校验生成文件与清单一致 | `python scripts/gen_ts_grammars.py --check` |

**新增语言流程**：在 `scripts/fetch_ts_grammars.py` 的 `COMMON_LANGS`（或 `gen_ts_grammars.py` 的 `GRAMMARS`）加一行 → `fetch_ts_grammars.py --update`（自动获取）或手工加 GRAMMARS 行 → `gen_ts_grammars.py` 重新生成 → `cmake --preset default` 重新 configure → 构建。

**产物**（均由 `gen_ts_grammars.py` 自动生成）：
- `cmake/ts_grammars.cmake` — `workx_fetch_ts_grammar(...)` fetch 调用列表
- `src/tui/render/ts_langs_decl.inc` — `extern "C"` 声明（`tree_sitter_<name>`）
- `src/tui/render/ts_langs_reg.inc` — GrammarRegistry 构造体注册块

**构建注意事项**：
- configure 期拉取全部 grammar 仓库（需网络）；commit hash 固定版本禁用 shallow clone 走全量克隆，首次 configure 很慢（数十分钟），缓存后约 2 分钟。
- `CMP0097=NEW` + `GIT_SUBMODULES ""` 禁用 submodule 初始化（ocaml/tlaplus 等仓库自带测试用 submodule）。
- 个别 grammar 的 scanner 定义非 static 通用函数（serialize/deserialize/scan），构建期自动宏重命名为 `<grammar>_<func>` 防链接冲突。
- 语言数受体积约束：当前保留 30 个主流语言（从 69 个裁剪），加回需接受 workx.exe 体积增大（grammar 静态链接进 `.rdata`）。

## 单元测试（按模块拆分）

单元测试已从单一 `workx_unit_tests` 拆分为 **5 个独立目标**，与 `src/` 分层对齐（每个目标只链接其依赖的最少库）：

| 目标 | 覆盖目录 | 链接库 |
|---|---|---|
| `core_unit_tests` | `tests/unit/core/**` | `workx_core` |
| `agent_unit_tests` | `tests/unit/agent/**`（含 `helpers` 自测） | `workx_agent` |
| `tui_unit_tests` | `tests/unit/tui/**` | `workx_tui` |
| `island_unit_tests` | `tests/unit/island/**` | `workx_island` |
| `app_unit_tests` | `tests/unit/app/**` | `workx_app` |

> 说明：`test_command_system.cpp` 物理位于 `agent/command/`，但依赖 app 层 `register_system_commands`，故归入 `app_unit_tests`。

| 目的 | 命令 |
|---|---|
| 配置（vcpkg toolchain） | `cmake --preset default` |
| 构建某个模块测试 | `cmake --build build --config Release --target <模块>_unit_tests -j 8` |
| 并行构建全部测试目标 | `cmake --build build --config Release -j 8` |
| 并行跑全部测试 | `ctest --test-dir build -C Release -j 8 --output-on-failure` |
| 只运行某模块测试 | `build/bin/Release/<模块>_unit_tests.exe`（或 `ctest -R <模块> -j 8`） |
| 快速回归（跳过 [slow] 慢测试） | `ctest --test-dir build -C Release -LE slow -j 8` |
| 只跑慢测试（验证超时/并发类逻辑） | `ctest --test-dir build -C Release -L slow -j 8` |
| 按功能标签过滤 | `ctest --test-dir build -C Release -L <tag> -j 8` |
| 按名称筛选 | `ctest --test-dir build -C Release -R "skill" -j 8` |
| 单测可执行文件按标签筛选 | `build/bin/Release/core_unit_tests.exe "[skill]"` |

说明：
- 模块测试源文件通过 `file(GLOB_RECURSE ... CONFIGURE_DEPENDS)` 自动收集，新增测试文件无需手改列表。
- `catch_discover_tests(ADD_TAGS_AS_LABELS ON)` 把每个测试的 Catch2 标签（含 `[slow]`）映射为 ctest 标签，故可用 `-L / -LE` 过滤。

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
