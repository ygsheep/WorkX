# Issue #21 库化改造实施计划

**目标**：把 `libworkx`（应用减 main 的伪库）重构为可被外部消费的 Agent Harness 库。
**基线**：`ed637ff`（develop）。
**范围**：M1 拆库边界+切反向依赖 → M2 构建产物库化 → M3 收敛公共 API 面。M4（动态扩展/插件/MCPTool）仅记录接口预留，不在此次实现。

## 现状核查（已实地确认）

- `src/CMakeLists.txt:9` 单一大杂烩 `libworkx STATIC`，core/agent/tui/app 四层全部 `target_sources` 追加
- 5 处反向依赖：
  | 位置 | 依赖 | 实际用途 |
  |---|---|---|
  | `agent/core/chat_session.cpp:17` | `app/config/app_config.h` | 仅 `agent::keys` 常量 |
  | `agent/tool/FileEditTool/file_edit_tool.cpp:31` | `app/config/app_config.h` | 仅 `agent::keys` |
  | `agent/tool/FileReadTool/file_read_tool.cpp:18` | `app/config/app_config.h` | 仅 `agent::keys` |
  | `agent/tool/AskUser/AskUserTool.cpp:15` | `tui/widgets/choice_panel.h` | `tui::parse_choice_config` + `tui::ChoiceResult` |
  | `agent/tool/FileWriteTool/file_write_tool.cpp:25` | `app/ui/file_index.h` | `global_file_index().mark_dirty()` |
- `core/events/agent_events.h:98` 事件携带 `std::promise<tui::ChoiceResult>`（仅前向声明，但类型仍是 tui 的）
- 关键事实：`app/config/app_config.{h,cpp}` 内容全在 `namespace agent`（配置键+Schema 注册），且 `.cpp` 只依赖 `core/config/*` 与 `agent/tool/constants.h`——**本质是 agent 层内容错放在 app 目录**；tree-sitter 仅被 `tui/render/syntax_highlighter.*` 使用——**归属 tui 层**
- 模板：`lib/liblogger/`（install/EXPORT/config.cmake.in 完整范例）

## M1 拆库边界 + 切断反向依赖（前提）

### M1a. app_config 迁移（消除 3 处反向依赖）

- `git mv src/app/config/app_config.{h,cpp} src/agent/config/`，namespace `agent` 不变，内部 include 改 `agent/config/app_config.h`
- 全仓 15 处 include 更新：agent 内 3 处变同层；`src/app/*`（factory/main/ui/*/cli_args）、`example/example_provider_form`、`tests/benchmarks`、`tests/unit` 5 处改指新路径（app→agent 是合法方向）
- `src/app/CMakeLists.txt`（若有）与 `src/agent/CMakeLists.txt` 源文件清单同步调整

### M1b. FileIndex 脏标记 → 宿主回调（消除 1 处）

- `ToolContext`（`agent/tool/context.h`）新增可选回调 `std::function<void()> on_file_system_changed`，默认空（无宿主 no-op），对齐既有 `progress_callback`/`touch_callback` 模式
- `ReActLoop` 构造新增可选参数透传 → `ctx.on_file_system_changed`；`ChatSession` 新增可选 setter `set_file_index_invalidator(std::function<void()>)`（默认空，向后兼容，不加构造参数避免破坏调用点）
- TUI/app 接线：main 初始化时注册 `[]{ global_file_index().mark_dirty(); }`；`file_write_tool.cpp` 改调 `ctx.on_file_system_changed`（有回调才触发），删除 `#include "app/ui/file_index.h"`

### M1c. AskUser 去 tui 化（消除 1 处 + core 事件类型剥离）

- `core/events/agent_events.h` 新增 agent 自有结果类型：
  ```cpp
  struct AskUserResult {
      bool submitted = false;
      std::vector<std::pair<std::string, std::string>> answers;
  };
  ```
  `AskUserRequestEvent::result_promise` 改为 `std::promise<AskUserResult>`；删除 `namespace tui { struct ChoiceResult; }` 前向声明
- 新增 `agent/tool/AskUser/ask_user_config.{h,cpp}`：纯函数 `bool validate_ask_user_json(const nlohmann::json&)`（实现=现有 `parse_choice_config` 的校验逻辑，与 TUI 渲染解耦）；AskUserTool 改用此校验，删除 tui include；超时/取消分支构造 `AskUserResult`
- TUI 侧保持 `tui::ChoiceResult` 自有类型：`chat_renderer.cpp:1029` 订阅回调与 `terminal.cpp` 的 `take_pending_ask` 处理在 `set_value` 时做 `tui::ChoiceResult → agent::AskUserResult` 转换（`to_json` 保留 tui 侧）
- 事件+promise 异步协议本身保留（这是工具线程→宿主主线程的正确 marshalling，宿主不订阅时工具走超时分支，循环仍可运行）

### M1d. 层边界编译期校验（验收项 2）

- 新增 `tests/unit/agent/test_layer_boundary.cpp`：用 `SOURCE_DIR` 宏遍历 `src/agent/`、`src/core/` 下 `.h/.cpp`，正则断言无 `#include "tui/`、`#include "app/`、`#include "example/`；core 额外禁止 `agent/`
- 单测直接跑通即达成"可编译期校验"

### M1e. 按层拆分库目标（拆库边界）

| 目标 | 内容 | 依赖 | tree-sitter |
|---|---|---|---|
| `workx_core` STATIC | `src/core/*` | nlohmann_json | 无 |
| `workx_agent` STATIC | `src/agent/*`（含迁移来的 `agent/config/app_config.cpp`） | workx_core、CURL::libcurl、nlohmann_json | 无 |
| `workx_tui` STATIC | `src/tui/*` | workx_agent、nlohmann_json | 从 libworkx 移入（runtime PUBLIC / grammar PRIVATE） |
| `workx` 可执行 | `app/main.cpp` + `src/app/{command,ui,factory.cpp}`（config/ 已迁出） | workx_tui | — |

- 删除 `libworkx` 聚合目标；`choice_preview`、`tests/unit`、`tests/integration`、`example/*` 引用改链 `workx_tui`（transitive 带上 agent/core）
- `src/CMakeLists.txt` 的 icon/ripgrep 逻辑保留在 `workx` 可执行目标
- 检查 `src/core/process/tool_registry.cpp` 是否真为 core 层内容（文件名可疑，若引用 agent 头则需定位调整）

## M2 构建产物库化

参照 `lib/liblogger/CMakeLists.txt`：

- `include(GNUInstallDirs)` + `install(TARGETS workx_core workx_agent EXPORT workx_targets ...)`，`NAMESPACE workx::`（`workx::core` / `workx::agent`）；workx_tui 为内部目标不安装
- 新 `workxConfig.cmake.in`：`@PACKAGE_INIT@` + `find_dependency(nlohmann_json)` + `find_dependency(CURL)`（两个都是公共头暴露的硬依赖）+ `include(workxTargets.cmake)` + `set(workx_LIBRARIES workx::agent)`
- 依赖必选化（PR 评审 P1/P2 修复）：构建侧与安装侧语义一致 —— 根 CMakeLists `find_package(nlohmann_json CONFIG REQUIRED)`（原 QUIET 静默失败 → 编译期 C1083），src/core、src/agent 无条件链接 json（删 `if(nlohmann_json_FOUND)` 死分支与 `WORKX_HAS_NLOHMANN_JSON` 死宏）。注意 add_subdirectory 消费模式下 vcpkg manifest 只对顶层生效，消费方须自带 vcpkg.json 声明依赖（`tests/consumer/vcpkg.json` 已提供 nlohmann-json + curl）
- `write_basic_package_version_file(... COMPATIBILITY SameMajorVersion)`，版本承诺锚定 `PROJECT_VERSION`（0.2.0）
- 公共头安装由 M3 白名单驱动，目标目录 `include/workx/...`
- `INSTALL_INTERFACE:include` 已存在于 libworkx 目标，拆分后逐个目标保留
- 消费验证：新增 `tests/consumer/` 冒烟工程（add_subdirectory 模式为主；`scripts/` 或 CI 步骤可选跑 install → find_package 模式）

## M3 收敛公共 API 面

- 新增 `include/workx/export.h`：`WORKX_API` 导出宏（WIN32 `__declspec(dllexport/import)` 语义，静态库下为空操作，为未来 DLL/插件预留）
- 公共头白名单 `WORKX_PUBLIC_HEADERS`（CMake 显式列表，**只安装白名单，未列入即私有**，物理目录不动，避免 1.7 万行搬移）。v1 最小集（实施时以"外部驱动 ReAct 循环最小面"核准）：
  - core：`events/{i_event_bus,event_bus,agent_events}.h`、`config/i_config_manager.h`、`task/i_task_manager.h`、`utils/{result,result_v2,error}.h`
  - agent：`core/{chat_session,react_loop,react_observer}.h`、`api/*.h`、`message/types.h`、`tool/{registry,executor,context}.h`、`session/session_store.h`、`config/app_config.h`、`tool/AskUser/ask_user_config.h`
- 头文件加 `WORKX_API` 标注公共符号（v1 静态库仅声明性标注）

## M4 动态扩展（记录，不实现）

- 插件机制：`PluginLoader`（dlopen/LoadLibrary + C ABI `workx_plugin_register_tools`）+ `PluginToolRegistry` 扫描插件目录 —— 接口设计与 `tool/registry.h` 对接点
- MCPTool：定义 `IMcpClient` 抽象（`agent/tool/MCPTool/`），默认实现从 `NotImplemented` 改为返回配置缺失错误；stdio JSON-RPC 客户端后置
- 本里程碑独立排期，不阻塞 M1-M3 验收

## 测试与验证

1. `[layer_boundary]` 单测：agent/core 零反向 include
2. 回归：`cmake --preset default && cmake --build build --config Debug --target workx_unit_tests && ctest --test-dir build -C Debug --output-on-failure`（重点 `[choice_panel]`、`[chat_session]`、`[tool]`）
3. consumer 冒烟：`tests/consumer/` 用 fake provider 驱动 ChatSession 跑一轮 ReAct 循环，仅订阅 EventBus 打印输出，**不链任何 tui/app 目标**
4. 可选：`cmake --install` 到临时目录 + consumer `find_package(workx CONFIG)` 编译链接
5. add_subdirectory 全量实测（PR 评审 P1）：`cmake -S tests/consumer -B <b> -DWORKX_SOURCE_DIR=<workx 根> -DCMAKE_TOOLCHAIN_FILE=<vcpkg> -DWORKX_FETCH_GRAMMARS=OFF` 完整编译（含 workx.exe）通过；顺带修复 tree-sitter=OFF 降级路径缺失 `highlight_diff` 定义的既有链接缺陷

## 进度记录（2026-08-06）

### 已完成：M1a–M1e、M2

- **M1a**：`git mv src/app/config/app_config.{h,cpp} → src/agent/config/`；14 文件 include 改指 + CMake 清单同步
- **M1b**：`ToolContext::on_file_system_changed` + `ReActLoop` 第 10 参 `file_index_invalidator` + `ChatSession::set_file_index_invalidator` + factory 接线 `global_file_index().mark_dirty()`
- **M1c**：`agent::AskUserResult` 入 `core/events/agent_events.h`；新 `agent/tool/AskUser/ask_user_config.{h,cpp}` 纯函数校验；`terminal.cpp` 三处 set_value 转换
- **M1d**：`tests/unit/agent/test_layer_boundary.cpp`（SOURCE_DIR 扫描，`[layer_boundary]` 7 assertions 通过）
- **M1e**：四层目标 `workx_core` → `workx_agent` → `workx_tui` → `workx_app`；新增 **`workx_app` 内部目标**（工厂函数：create_session/make_terminal_config/register_builtin_tools，单测与 choice_preview/example_provider_form 需要）；`libworkx` 删除
- **额外发现并修复**：`file_search_panel`（tui）也 include `app/ui/file_index.h` → FileIndex 下移到 `core/utils/file_index.{h,cpp}`（第 6 处越层）
- **M2**：install/EXPORT/version + `src/workxConfig.cmake.in`（`find_dependency(nlohmann_json/CURL/logger)`）；公共头按目录结构安装到 `include/` 根（`install(FILES)` 会拍平子目录，用 foreach 按目录分组）
- **liblogger 修复**（外部消费阻塞项）：
  - `loggerConfig.cmake.in` 原为 deartsdl 残留模板（引用 `deartsdl_loggerTargets.cmake` + `find_dependency(SDL3)`）→ 改为 `logger_targets.cmake` + `logger_LIBRARIES=logger`
  - 导出目标无 NAMESPACE（与构建树同名 `logger`，workx 导出引用才能解析）；头安装到 `include/liblogger/`（原 `include/logger/` 与 `<liblogger/logger.h>` 风格不符）
  - 教训：别名 `logger::logger` 会被 CMake 导出为 `loggerlogger`（别名目标导出拼接 bug），不可用
- **白名单闭包**：按 chat_session.h 传递闭包补齐 14 个头（`agent/model/provider_type.h`、`agent/compact/*`、`agent/command/inclaude/*`、`agent/skill/inclaude/conditional.h`、`core/events/{event_token,events,stream_events,system_events}.h`、`core/task/thread_pool.h`、`core/tool_kind.h`、`agent/tool/{itool,result}.h`）
- **消费验证全绿**：仓库内（WORKX_BUILD_CONSUMER=ON）✓ / 外部 add_subdirectory（独立 configure，`-DWORKX_SOURCE_DIR`，`WORKX_FETCH_GRAMMARS=OFF`）✓ / 外部 find_package（`cmake --install` → `workx::agent`）✓ —— 三模式均输出 `OK: agent loop driven without TUI`
- **导出名去重**（`9fbfd44`）：`EXPORT_NAME` 令安装树目标为 `workx::agent` / `workx::core`（内部目标名 workx_agent/workx_core 不变）；`workx_LIBRARIES=workx::agent workx::core`
- 回归：790 单测 = 789 通过 / 1 失败（`BashTool reports non-zero exit code`，Windows `exit /b 42` 环境性，既有失败）
- 已知约束：多配置消费者（如 VS 全配置生成）在只安装了 Debug 时会报 `IMPORTED_LOCATION not set` —— 需 `-DCMAKE_CONFIGURATION_TYPES=Debug` 或安装全部构建配置

### 待办：M3（导出宏 + 白名单收尾）

- ~~新增 `src/export.h` 或 `include/workx/export.h`：`WORKX_API` 宏，加入 `WORKX_PUBLIC_HEADERS`~~ ✅ 已完成：`src/core/export.h`（WIN32 dllexport/import + Unix visibility，v1 静态库空操作），列入白名单
- ~~公共头加 `WORKX_API` 标注公共符号（v1 静态库声明性标注）~~ ✅ 已完成：15 处核心类/函数（IEventBus/EventBus、IConfigManager/ConfigManager、ITaskManager/TaskManager、ChatSession/ReActLoop/IReActObserver、ICompletionProvider/IStreamReader、ToolRegistry/ToolExecutor、SessionStore、validate_ask_user_json）；header-only struct/数据事件不标（无外部符号，DLL 场景亦无需导出）
- 白名单按"外部驱动 ReAct 最小面"复核：现为传递闭包全集（chat_session.h 的 include 闭包），保持现状不再收敛（收敛需改动公共头 include 结构，收益低）

**Issue #21 全部完成**：M1a–M1e（分层拆库 + 零反向依赖 + 边界单测）、M2（install/EXPORT + 三模式消费验证）、M3（export.h + WORKX_API 标注）。回归 789/790（1 个 Windows 环境性既有失败）。


## 验收标准映射

- 外部工程 `find_package(workx)`/`add_subdirectory` 链接并驱动 Agent 循环，无 TUI 依赖 → M1e + M2 + M3（consumer 冒烟验证）
- agent 层零反向依赖（编译期校验）→ M1a-d（层边界单测）
- 库 API 面有明确公共头清单与版本承诺 → M2 version 文件 + M3 白名单 + `WORKX_VERSION` 宏

## 风险与决策点

- `app_config` 迁移触碰 15 处 include——机械替换，低风险
- `ToolContext`/`ChatSession` 新增可选参数——向后兼容（默认空）
- AskUser 协议类型变更需同步 `chat_renderer.cpp`/`terminal.cpp`——已有 choice_panel 单测兜底
- 拆分目标名变更触及 tests/example 引用——仓库内可控
- tree-sitter 移入 tui 后，workx_agent 包依赖收敛为 nlohmann_json + CURL，包配置简单
- 是否保留 `libworkx` ALIAS 兼容名：否（仓库内部可控，避免长期双名）
