# Workx

<div align="center">

<img src="src/icon.png" alt="Workx Icon" width="128" height="128"/>

**基于 ReAct 循环与工具调用架构的现代终端 Code Agent / Work Agent，能够自主完成编码、调试、文件操作与任务编排**

[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![Platform](https://img.shields.io/badge/Platform-Windows%20%7C%20Linux%20%7C%20NixOS-2ea44f.svg)](#构建步骤)
[![License](https://img.shields.io/badge/license-MIT-purple.svg)](LICENSE)
[![CI](https://github.com/ygsheep/WorkX/actions/workflows/code-quality.yml/badge.svg)](https://github.com/ygsheep/WorkX/actions/workflows/code-quality.yml)

[功能特性](#特性) • [快速开始](#构建步骤) • [运行](#运行) • [命令参考](#命令参考) • [配置](#配置) • [测试](#测试)

</div>

---

> 一个现代化的终端 Code Agent / Work Agent，基于 ReAct 循环与工具调用架构，能够自主完成编码、调试、文件操作与任务编排。
>
> **Agent Harness 定位**：除终端客户端外，`src/core` + `src/agent` 构成可复用的 Agent Harness 库（`workx::agent`）。外部工程可通过 `find_package(workx)` 或 `add_subdirectory` 链接并驱动 ReAct Agent 循环（注入 `ICompletionProvider`、订阅 `EventBus` 事件即可），**无需任何 TUI/应用层依赖**——`src/tui` 与 `src/app` 只是参考宿主实现。消费示例见 `tests/consumer/`。

## 特性

- **自主任务执行**: 基于 ReAct 循环（推理 + 行动），Agent 自主规划并调用工具完成任务，而非简单问答
- **工具调用能力**: 内置文件读写、编辑、Glob/Grep 搜索、Bash 执行、Web 抓取、子 Agent 调度等工具
- **多提供商支持**: 内置 OpenAI、Anthropic、DeepSeek 等预设，一键切换
- **权限控制系统**: 对敏感工具调用进行权限校验，支持多种权限模式
- **上下文管理**: 自动 Token 压缩与记忆管理，长任务也能保持连贯上下文
- **MCP 协议集成**: 通过 Model Context Protocol 接入外部数据源与工具
- **SSE 流式响应**: 实时显示 Agent 推理与工具调用过程
- **Markdown 渲染**: 支持表格、标题、列表、代码块（Tree-sitter 语法高亮，支持 30 种语言）、强调等完整 Markdown 语法
- **Diff 可视化**: 文件编辑以带背景色的 Diff 形式呈现，清晰展示增删改动
- **命令系统**: `/help`、`/exit`、`/clear`、`/regen`、`/model` 等内置命令
- **自定义模型**: 支持添加自定义模型和 API 端点
- **彩色输出**: 终端彩色渲染，语法高亮的代码块

## 架构概览

![Workx 四层架构图（鲸鱼娘解说版）](docs/architecture_overview.jpg)

一个现代化的终端 Code Agent / Work Agent，基于 ReAct 循环与工具调用架构，能够自主完成编码、调试、文件操作与任务编排。

**Agent Harness 定位**：除终端客户端外，`src/core` + `src/agent` 构成可复用的 Agent Harness 库（`workx::agent`）。外部工程可通过 `find_package(workx)` 或 `add_subdirectory` 链接并驱动 ReAct Agent 循环（注入 `ICompletionProvider`、订阅 `EventBus` 事件即可），**无需任何 TUI/应用层依赖**——`src/tui` 与 `src/app` 只是参考宿主实现。消费示例见 `tests/consumer/`。

### 核心工作流（鲸鱼娘图解）

| 📐 ReAct 推理循环 | 📡 EventBus 跨层中枢 |
|---|---|
| ![ReAct 推理循环](docs/img/01_react_loop.jpg) | ![EventBus 跨层事件中枢](docs/img/02_eventbus_flow.jpg) |
| Thought → Action → Observe 循环 ≤25 轮，无工具调用即输出 FinalAnswer | 发布-订阅解耦四层：TUI/Agent/Core 异步消息驱动，同步 publish 需防死锁 |

```
src/
├── agent/              # Agent 核心层
│   ├── api/            # LLM 后端接口（OpenAI/Anthropic 适配器、SSE 流、HTTP 客户端）
│   ├── command/        # 命令执行器与注册表
│   ├── compact/        # Token 压缩与上下文截断
│   ├── core/           # 会话管理、查询引擎、ReAct 推理循环
│   ├── input/          # 输入解析与处理
│   ├── message/        # 消息构建与历史管理
│   ├── model/          # 模型配置、提供商预设、模型路由
│   ├── permission/     # 权限校验、权限模式、规则定义
│   ├── prompt/         # 系统提示词与记忆管理
│   ├── tool/           # 工具集（Agent 自主调用的能力）
│   │   ├── AgentTool/      # 子 Agent 调度（任务分解与并行执行）
│   │   ├── BashTool/       # Shell 命令执行
│   │   ├── FileEditTool/   # 文件精确编辑
│   │   ├── FileReadTool/   # 文件读取
│   │   ├── FileWriteTool/  # 文件写入（含 Diff 生成）
│   │   ├── GlobTool/       # 文件名模式匹配
│   │   ├── GrepTool/       # 内容正则搜索
│   │   ├── MCPTool/        # Model Context Protocol 工具桥接
│   │   └── WebFetchTool/   # 网页内容抓取
│   └── util/           # 异步、JSON Schema、字符串工具
├── app/                # 应用层
│   ├── command/        # 内置系统命令
│   ├── config/         # 配置管理、CLI 参数解析
│   ├── ui/             # 模型选择器、路径补全、文件索引
│   └── main.cpp        # 程序入口
├── core/               # 核心基础设施
│   ├── config/         # 配置管理器
│   ├── events/         # 事件总线（驱动 Agent 与 UI 解耦）
│   ├── task/           # 任务管理器
│   └── utils/          # Result 类型等通用工具
└── tui/                # 终端用户界面
    ├── core/           # 终端封装、屏幕管理、显示缓冲（Win32/POSIX）
    ├── input/          # 行编辑器、输入历史
    ├── render/         # 聊天渲染、Markdown 渲染、Tree-sitter 语法高亮、流式缓冲
    ├── setup/          # 设置向导
    ├── utils/          # UTF-8 工具函数
    └── widgets/        # 状态栏、命令面板、文件搜索面板、底部栏
```

## 支持的 AI 提供商

仅内置中国顶级模型厂商预设，另支持自定义 OpenAI 兼容端点：

| 提供商 | 预设名称 | 默认模型 |
|--------|----------|----------|
| DeepSeek | `deepseek` | deepseek-v4-flash |
| 智谱 GLM | `glm` | glm-5.2 |
| Kimi (Moonshot) | `kimi` | kimi-k3 |
| 通义千问 (Qwen) | `qwen` | qwen-plus |
| MiniMax | `minimax` | MiniMax-M3 |
| Custom URL | `openai-compatible` | (自定义) |

![LLM 后端适配层（插件式 8 家 Provider）](docs/img/08_backend_adapter.jpg)

> 架构分层：上层 `ReActLoop/ChatSession` 只依赖 `ICompletionProvider + IBackendAdmin` 双接口，新增 Provider 只需实现一个 Adapter。

## Agent 工具能力

WorkX 的核心是 Agent 自主调用工具完成任务。内置工具集如下：

| 工具 | 说明 |
|------|------|
| `FileReadTool` | 读取文件内容，支持行范围与偏移 |
| `FileWriteTool` | 创建或覆盖文件，并生成 Diff |
| `FileEditTool` | 基于字符串替换的精确编辑 |
| `GlobTool` | 按文件名 glob 模式快速查找文件 |
| `GrepTool` | 基于 ripgrep 的内容正则搜索 |
| `BashTool` | 执行 Shell 命令（受权限系统管控） |
| `WebFetchTool` | 抓取并提取网页内容 |
| `AgentTool` | 启动子 Agent 处理子任务，支持并行调度 |
| `MCPTool` | 桥接 MCP 服务器，接入外部工具与数据源 |

![工具调用全管线（权限 + 密钥脱敏 + 并行执行）](docs/img/04_tool_pipeline.jpg)

> 执行顺序：Registry 查表 → 构造 ToolContext → Permission 校验 Ask/Auto/Deny → SecretScanner 脱敏 → TaskManager 并行 invoke → 结果回写 messages。
>
> **MCP 桥接：** 通过 JSON-RPC 跨进程调用第三方 MCP Server，无需改 Workx 本体即可扩展外部工具。
>
> ![MCP 跨进程工具桥接](docs/img/09_mcp_bridge.jpg)

## 上下文管理 & Token 压缩

![Token 上下文压缩 + 预算面板](docs/img/05_token_compression.jpg)

- **触发条件**：总 Token 用量超过上下文窗口 90%（优先级：provider → cfg → capability → preset → default）
- **永不裁剪区**：system prompt + 最近 5 轮对话 + 上一轮 tool_calls/results
- **兜底策略**：中段历史摘要压缩 → 依然超限降级多工具并行 → 最后抛 `ContextOverflowError`
- **实时面板**：StatusBar 显示 4 类计数（Prompt / Cache Create / Cache Read / Generated）+ 估算费用

## 前置条件

![项目依赖全景图（核心库 + 可选库分层说明）](docs/img/10_dependency_overview.jpg)

- **CMake**: 3.21 或更高版本
- **C++ 编译器**: 支持 C++20（MSVC 14.5+ 或 GCC 10+）
- **vcpkg**: 依赖包管理器，依赖库如下：

| 库 | 说明 |
|----|------|
| `nlohmann-json` | JSON 解析（所有平台） |
| `catch2` | 单元测试框架（所有平台） |
| `curl` | HTTP 客户端（所有平台） |
| `tree-sitter` | 语法高亮（所有平台） |
| `imgui` | 调试用原生窗口（仅 Windows，`dx11-binding` + `win32-binding`） |

## 构建步骤

> **注意**: Windows 为完整支持平台；Linux（含 NixOS）亦受支持，NixOS 安装见下文 Flake 章节。

![CMake 跨平台构建管线（vcpkg + FetchContent 一键三平台）](docs/img/07_build_pipeline.jpg)

### Linux
```bash
# 初始化 vcpkg 依赖（首次使用，${VCPKG_ROOT} 为你的 vcpkg 安装路径）
${VCPKG_ROOT}/vcpkg install nlohmann-json catch2 curl tree-sitter

# 配置 CMake
cmake -B build -DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake

# 构建
cmake --build build
```

### Windows (MSVC)

```bash
# 初始化 vcpkg（首次使用）
vcpkg install nlohmann-json catch2 curl

# 创建构建目录
mkdir build
cd build

# 配置 CMake（替换 [vcpkg_root] 为你的 vcpkg 安装路径）
cmake .. -DCMAKE_TOOLCHAIN_FILE=[vcpkg_root]/scripts/buildsystems/vcpkg.cmake

# 构建
cmake --build . --config Release
```

## 语法高亮（Tree-sitter）

代码块语法高亮基于 **Tree-sitter**（runtime 由 vcpkg 提供，grammar 仓库通过 CMake FetchContent 在 configure 期从 GitHub 拉取），支持 **30 种主流语言**（c/cpp/c#/python/js/ts/go/rust/java/kotlin/swift/ruby/php/bash/json/yaml/toml/markdown/html/css/lua/dockerfile/cmake/make/ini/dart/scala/haskell/perl）。语法高亮开关：`WORKX_WITH_TREE_SITTER`（默认 ON）；Nix 无网络环境设 `WORKX_FETCH_GRAMMARS=OFF` 降级为 no-op。

> **体积说明**：tree-sitter grammar 以**静态库全量链接**进 workx.exe，体积主要来自各 grammar 的解析状态表（`.rdata` 只读数据段），与语法复杂度成正比。当前裁剪到 30 个主流语言后，workx.exe 约 **44MB**（此前支持 69 种时约 97MB）。如需更多语言，可在 `GRAMMARS` 清单加回（体积会相应增大）；也可评估 DLL 动态加载方案（改动较大）。

**语言清单与自动生成**：

- 语言清单集中维护在 [`scripts/gen_ts_grammars.py`](scripts/gen_ts_grammars.py) 的 `GRAMMARS` 列表，运行该脚本自动生成 3 处注册代码：`cmake/ts_grammars.cmake`（fetch 调用）、`src/tui/render/ts_langs_decl.inc`（extern "C" 声明）、`src/tui/render/ts_langs_reg.inc`（注册块）。
- [`scripts/fetch_ts_grammars.py`](scripts/fetch_ts_grammars.py) 可从 tree-sitter 官方 wiki 自动抓取可用 grammar（过滤 github 仓库 + ABI≥14 + 已预生成 parser.c），并用 `git ls-remote` 解析默认分支 HEAD commit，`--update` 直接更新 `GRAMMARS`。

**新增一种语言**（以 foo 为例）：
```bash
# 方式一：从 wiki 自动获取（推荐）
python scripts/fetch_ts_grammars.py --list          # 查看候选
python scripts/fetch_ts_grammars.py --update        # 更新 GRAMMARS（需先在 COMMON_LANGS 加 foo）

# 方式二：手工在 gen_ts_grammars.py 的 GRAMMARS 加一行
python scripts/gen_ts_grammars.py                   # 重新生成注册代码

cmake --preset default                              # 重新 configure（拉取新 grammar 仓库）
cmake --build build --config Release -j 8           # 构建
```

**构建注意事项**：
- configure 期会拉取全部 grammar 仓库（需要网络）。多数 grammar 以 commit hash 固定版本，**禁用 shallow clone 走全量克隆**，首次 configure 可能耗时数十分钟；仓库已缓存后再次 configure 约 2 分钟。
- 部分 grammar 仓库自带测试用 submodule（如 ocaml 的 examples/*、tlaplus 的 test/*），CMake 已设 `CMP0097=NEW` + `GIT_SUBMODULES ""` 禁用其初始化，避免拉取海量无用代码。
- 个别第三方 grammar 的 scanner 定义了非 static 的通用函数（serialize/deserialize/scan），CMake 构建期自动检测并用宏重命名为 `<grammar>_<func>`，避免链接符号冲突。
- 曾因 MSVC 兼容性/仓库结构复杂排除 crystal、typst、ocaml；本次体积裁剪又移除了 verilog/fortran/nim/tlaplus/zig 等冷门大体积语言。如需加回，直接在 `GRAMMARS` 加一行即可。

## Nix / NixOS 安装

> 不依赖 vcpkg/FetchContent：`nix/` 目录下的 `workx.nix` + `workx.patch` 将依赖全部换成 nixpkgs 提供。
> 其中 `workx.patch` 对 nlohmann_json 3.12.0 打上 vcpkg 同款补丁（官方版在 GCC 下 `value("key", std::optional<...>)` SFINAE 实例化失败）。

### 方式一：flake（推荐，NixOS 系统安装）

在 NixOS 系统的 `flake.nix` 中加入 input：

```nix
{
  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
    workx = {
      url = "github:ygsheep/WorkX/develop"; # 指定 develop 分支为源码
      inputs.nixpkgs.follows = "nixpkgs";   # 对齐系统 nixpkgs
    };
  };

  outputs = { self, nixpkgs, workx, ... }@inputs: {
    nixosConfigurations.myhost = nixpkgs.lib.nixosSystem {
      system = "x86_64-linux";
      modules = [
        ({ pkgs, ... }: {
          environment.systemPackages = [
            inputs.workx.packages.${pkgs.system}.default
          ];
        })
      ];
    };
  };
}
```

home-manager 应用同理：`home.packages = [ inputs.workx.packages.${pkgs.system}.default ];`

本仓库内可直接试用：`nix build .#default` 或 `nix profile install .#default`。

### 方式二：callPackage（home-manager / NixOS 直接引用）

将 `nix/workx.nix` 与 `nix/workx.patch` 拷入配置仓库（保持同目录），`src` 指向源码：

```nix
{ pkgs, ... }:
{
  home.packages = [
    (pkgs.callPackage ./nix/workx.nix {
      src = pkgs.fetchFromGitHub {
        owner = "ygsheep";                  # 仓库实际 owner/repo
        repo = "WorkX";
        rev = "develop";                    # 分支或 tag
        hash = "sha256-...";                # 先用占位,构建报错后填真实值
      };
    })
  ];
}
```

> **说明**
> - Nix 构建沙箱无网络，tree-sitter runtime/grammars 的 FetchContent 拉取失败，故设 `WORKX_FETCH_GRAMMARS=OFF`，语法高亮降级为 no-op（渲染管线不受影响）。
> - CMake 的 install 规则只安装 `workx_core` / `workx_agent` 库，可执行文件在 `postInstall` 手动拷贝（含 `icon.png` / `tools/rg`）。
> - ripgrep 通过 `wrapProgram` 注入 PATH：workx 按 `bundled(<exe_dir>/tools/rg)` > PATH 顺序解析，Nix 下走 PATH 分支。

## 运行

![Setup Wizard 首次启动引导（4 步配置）](docs/img/06_setup_wizard.jpg)

```bash
# Windows
build\bin\workx.exe

# Linux/macOS
./build/bin/workx
```

首次运行时会启动设置向导，引导您配置 API Key 和选择默认模型。

## 命令参考

| 命令 | 说明 |
|------|------|
| `/help` | 显示所有可用命令 |
| `/exit` / `/quit` | 退出程序 |
| `/clear` | 清空聊天历史 |
| `/regen` | 重新生成上一条回复 |
| `/model` | 切换当前使用的模型 |

## TUI 渲染注意事项（开发必读）

![TUI Resize/Overlay 渲染管线（严格按此顺序，否则光标乱飞/快照失效/死锁）](docs/img/03_render_pipeline.jpg)

## 配置

配置文件位于用户目录下，包含 API Key、默认模型、超时设置等。设置向导会在首次运行时生成配置。

## 测试

单元测试已**按模块拆分为 5 个独立目标**，与 `src/` 分层对齐，解决编译慢与运行慢的问题（改某模块只重编/重链该模块，各模块可并行编译与运行）：

| 目标 | 覆盖目录 | 链接库 |
|---|---|---|
| `core_unit_tests` | `tests/unit/core/**` | `workx_core` |
| `agent_unit_tests` | `tests/unit/agent/**`（含 `helpers` 自测） | `workx_agent` |
| `tui_unit_tests` | `tests/unit/tui/**` | `workx_tui` |
| `island_unit_tests` | `tests/unit/island/**` | `workx_island` |
| `app_unit_tests` | `tests/unit/app/**` | `workx_app` |

```bash
# 构建某个模块测试（<模块> 为 core/agent/tui/island/app）
cmake --build build --config Release --target <模块>_unit_tests -j 8

# 并行运行全部测试
ctest --test-dir build -C Release -j 8 --output-on-failure

# 快速回归：跳过 [slow] 慢测试（验证超时/并发类逻辑被打上 [slow] 标签）
ctest --test-dir build -C Release -LE slow -j 8

# 只跑慢测试
ctest --test-dir build -C Release -L slow -j 8

# 按功能标签或名称过滤
ctest --test-dir build -C Release -L <tag> -j 8
ctest --test-dir build -C Release -R <name> -j 8

# 单测可执行文件按 Catch2 标签过滤
build/bin/Release/core_unit_tests.exe "[skill]"
```

> 源码自动收集：模块测试源文件经 `file(GLOB_RECURSE ... CONFIGURE_DEPENDS)` 收集，新增测试文件无需手改 CMake 列表；`catch_discover_tests(ADD_TAGS_AS_LABELS ON)` 将 Catch2 标签（含 `[slow]`）映射为 ctest 标签，从而支持 `-L / -LE` 过滤。

```bash
# 运行集成测试 (需启用 -DWORKX_BUILD_INTEGRATION_TESTS=ON, 且需 LM Studio)
build/bin/workx_integration_tests.exe
```

项目包含 900+ 个测试用例，覆盖核心功能模块。

## 许可证

MIT License
