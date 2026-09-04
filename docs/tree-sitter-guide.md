# Tree-sitter 语法高亮完全指南

代码块语法高亮基于 **Tree-sitter**（runtime 由 vcpkg 提供，grammar 仓库通过 CMake FetchContent 在 configure 期从 GitHub 拉取），支持 **30 种主流语言**：

`c / cpp / c# / python / js / ts / go / rust / java / kotlin / swift / ruby / php / bash / json / yaml / toml / markdown / html / css / lua / dockerfile / cmake / make / ini / dart / scala / haskell / perl`

| 开关 CMake Option          | 默认   | 说明                                                                        |
| ------------------------ | ---- | ------------------------------------------------------------------------- |
| `WORKX_WITH_TREE_SITTER` | `ON` | 全局启用/关闭语法高亮（关闭后渲染管线走 no-op fallback）                                      |
| `WORKX_FETCH_GRAMMARS`   | `ON` | configure 期是否通过 FetchContent 从 GitHub 拉各 grammar 仓库；**Nix 沙箱无网络必须设为 OFF** |

***

## 体积说明

tree-sitter grammar 以**静态库全量链接**进 `workx.exe`（或 Linux/macOS 下的 `workx` 二进制），体积主要来自各 grammar 的解析状态表（`.rdata` 只读数据段），与语法复杂度成正比。

- 当前裁剪到 30 个主流语言后：workx.exe ≈ **44 MB**

- 历史版本支持 69 种语言时：≈ 97 MB

如需更多语言，可在 `scripts/gen_ts_grammars.py` 的 `GRAMMARS` 清单加回（体积会相应增大）；也可评估 DLL 动态加载方案（改动较大，未在当前 roadmap）。

***

## 语言清单与自动生成

语言清单**集中维护**在 [`scripts/gen_ts_grammars.py`](../scripts/gen_ts_grammars.py) 的 `GRAMMARS` 列表。

**运行** **`scripts/gen_ts_grammars.py`** **自动生成 3 处注册代码**（手工改任何一处都容易漏，必须走脚本）：

| 生成文件                               | 内容                                                              |
| ---------------------------------- | --------------------------------------------------------------- |
| `cmake/ts_grammars.cmake`          | FetchContent 声明（每个 grammar 仓库 URL + commit hash）                |
| `src/tui/render/ts_langs_decl.inc` | 每个 grammar `TSLanguage *tree_sitter_<lang>()` 的 `extern "C"` 声明 |
| `src/tui/render/ts_langs_reg.inc`  | 注册块：`lang_registry.register("cpp", tree_sitter_cpp())` 等        |

***

## 抓取候选 & 更新 GRAMMARS（fetch\_ts\_grammars.py）

[`scripts/fetch_ts_grammars.py`](../scripts/fetch_ts_grammars.py) 可从 tree-sitter 官方 wiki 自动抓取可用 grammar：

- 过滤条件：GitHub 仓库 + ABI ≥ 14 + 已预生成 `parser.c`（免除 Node.js 构建工具链）

- 用 `git ls-remote` 解析默认分支 HEAD commit，保证版本一致性

- `--update` 参数可直接写回 `gen_ts_grammars.py` 的 `GRAMMARS`

```bash
# 查看可用候选（不修改任何文件）
python scripts/fetch_ts_grammars.py --list

# 自动更新 GRAMMARS（先手工把目标语言加入 COMMON_LANGS 列表）
python scripts/fetch_ts_grammars.py --update
```

***

## 新增一种语言（以 `foo` 为例）

### 方式一：从 wiki 自动获取（推荐）

```bash
# 1. 先确认候选
python scripts/fetch_ts_grammars.py --list | grep foo
# 2. 在 scripts/gen_ts_grammars.py 的 COMMON_LANGS 列表手工加 "foo"
# 3. 抓取并写回 GRAMMARS（含 commit hash）
python scripts/fetch_ts_grammars.py --update
# 4. 重新生成 3 处注册
python scripts/gen_ts_grammars.py
# 5. CMake configure + 构建
cmake --preset default
cmake --build build --config Release -j 8
```

### 方式二：手工维护

```bash
# 1. 直接在 scripts/gen_ts_grammars.py 的 GRAMMARS 加一行（含 owner/repo/commit）
# 2. 重新生成 3 处注册
python scripts/gen_ts_grammars.py
# 3. CMake + 构建
cmake --preset default
cmake --build build --config Release -j 8
```

***

## 构建注意事项

1. **首次 configure 需要网络**：FetchContent 会拉取全部 30 个 grammar 仓库。多数以 commit hash 固定版本，且 **禁用 shallow clone 走全量克隆**（避免"detached HEAD + 浅克隆 + 带 submodule 仓库"组合在 Windows/旧 Git 下失败），首次 configure 可能耗时数十分钟；仓库缓存后（`build/_deps/`）再次 configure ≈ 2 分钟。

2. **Grammar 自带 submodule 被禁用**：部分仓库（历史上 tlaplus test/、ocaml examples/）带 GB 级测试用例 submodule，CMake 全局设：

   ```cmake
   cmake_policy(SET CMP0097 NEW)           # ExternalProject 不自动 init submodule
   set(GIT_SUBMODULES "" CACHE STRING "")   # 传入 FetchContent 时传空列表
   ```

   避免拉取无用代码。

3. **链接符号冲突自动重命名**：个别第三方 grammar 的 `scanner.c` 定义了非 `static` 的通用函数（`serialize` / `deserialize` / `scan`），多个 grammar 一起链接会 ODR 冲突。CMake 构建期自动检测并用宏重命名为 `<grammar>_<func>`，无需手工改 vendored 源码。

4. **已排除语言清单**：

   - 曾因 MSVC 兼容性/仓库结构复杂排除：`crystal`、`typst`、`ocaml`

   - 本次体积裁剪移除（冷门 + 单 grammar > 2MB）：`verilog`、`fortran`、`nim`、`tlaplus`、`zig`

   - 如需加回：直接在 `GRAMMARS` 加一行即可，`gen_ts_grammars.py` 会生成所有注册代码。

