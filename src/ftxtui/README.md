# codex — 实验 FTXUI TUI

基于 FTXUI 的双栏实验终端 UI（与 `src/tui`（workx_tui）完全隔离：不链接、不 include）。
复用 `workx_agent` + `workx_core`（EventBus / ChatSession / ReActLoop），设计见
`docs/plans/2026-08-17-ftxui-tui-design.md`。

## 项目内容

双栏布局：左侧聊天转录，右侧侧边栏（模型 / 项目 / 会话标题），底部输入区内嵌状态行。

- **会话链路**：复用完整 ReAct 循环，注册最小工具集（FileRead / FileWrite / FileEdit /
  Bash / Glob / Grep / AskUser，Windows 额外含 PowerShell）。
- **Markdown 渲染**：标题/列表/加粗/删除线/行内代码/表格等；代码块带语法高亮
  （轻量实现，不依赖 tree-sitter）与语言标签。
- **流式输出**：LLM 正文与思考（reasoning）分别流式渲染，思考可折叠；自动跟随滚动 +
  滚轮/翻页手动滚动。
- **会话持久化**：JSONL 实时追加；`/resume` 恢复历史会话、`/rename` 改标题。
- **命令面板**：`Ctrl+P` 呼出，汇集本地命令与注册表命令。
- **AskUser 模态**：阻塞式提问（Enter 确认 / Esc 取消 / 超时自动关闭）。
- **权限模式**：`Shift+Tab` 切换 default / plan / bypass。
- **模型选择器**：`/model` 拉取并切换模型。
- **配色**：统一主题，见下文「主题（Theme）统一规范」。
- **mock 模式**：`--mock` 启动，用本地模拟流演示渲染与交互，无需后端。

内置命令：`/help` `/clear` `/exit` `/model` `/resume` `/rename`。
快捷键：`Enter` 发送 · `Ctrl+P` 命令面板 · `Shift+Tab` 权限 · `Ctrl+O` 折叠思考。

## 运行

构建目标为 `codex`（见 `CMakeLists.txt`）。`--mock` 可离线体验 UI；
真实模式需在配置中提供 provider / 远程地址 / 模型等。

## 目录结构

```
src/ftxtui/
├── main.cpp              入口：会话装配 + 输入管线接线
├── app.cpp/h             主循环、布局、命令分发、事件派发
├── bridge/               EventBus → UI action 队列桥接
├── vm/                   视图模型（消息树/面板状态）
├── command/              命令面板数据模型 + 注册表（ftxtui 独立）
├── theme/                统一主题：颜色 + 常用间距
├── render/               markdown 渲染 + 轻量语法高亮
└── widgets/              侧边栏 / 输入区 / 状态行 / 命令面板 / 模型选择
```

## 主题（Theme）统一规范

全站主题定义在 [`theme/theme.h`](src/ftxtui/theme/theme.h)。**新代码必须引用主题常量，
禁止散落硬编码 `RGB(...)` / `Color::Gray*`。**

> 背景以深灰（`#1e1e1e`）为主，故主体文字统一米白（`#F5F5F0`），避免「灰底灰字」低对比。

| 令牌 | 取值 | 用途 |
|---|---|---|
| `T::Canvas`   | `#000000` | 最外层画布背景 |
| `T::Panel`    | `#1e1e1e` | 面板 / 侧栏 / 输入区 / 代码块 / 用户消息 |
| `T::Surface`  | `#262626` | 转录区 / 卡片（比 Panel 略高一层） |
| `T::Selection`| `#2d3a55` | 选中项背景 |
| `T::Accent`   | `#5c9cf5` | 光标 / 左边框 / 搜索图标等强调 |
| `T::Text`     | `#F5F5F0` | 主文字（米白） |
| `T::TextDim`  | `#C8C8C8` | 次级文字（可读灰） |
| `T::TextFaint`| `#8E8E8E` | 弱提示 / 时间戳 / 边框 |

**语义色不动**：权限状态、语法高亮、思考动画、工具成败等用 `Color::*`
（如 `Color::YellowLight / GreenLight / RedLight`），保持独立命名，不入主题。

### 常用间距（统一）

- `theme::vPad(e)` —— 上下各一空行
- `theme::hPad(e)` —— 左右各两个空格
- `theme::pad(e)` —— 四周留白（上下一空行 + 左右两空格）
- `theme::panel(e)` —— 施加 `Panel` 背景

### 规范要点

1. 背景一律取 `T::Canvas / Panel / Surface / Selection`，不要写死 `RGB(...)`。
2. 主内容文字用 `T::Text`（米白）；只有时间戳/弱提示才用 `T::TextFaint`。
3. 需要「上下空行 + 左右两格」留白时调用 `theme::vPad / hPad / pad`。
4. 语法高亮（`render/syntax_highlight.cpp`）与状态行（`widgets/status_line.cpp`）
   属于语义色，保持独立命名，不并入主题表。

## 依赖

- **FTXUI v7.0.3**（FetchContent 拉取，见 `CMakeLists.txt`；不进入 vcpkg manifest）。
  > 曾从 v5.0.0 因终端乱码升级而来，**不要回退到 v5.x**。

## 代码适配要点（v7）

- **头文件拆分**：`ftxui::Event` 拆到独立头
  `ftxui/component/event.hpp`（`component.hpp` 不再传递包含）。
  用到 `Event::Xxx` 的 .cpp 需显式 include：`app.cpp`、`widgets/composer.cpp`、
  `widgets/model_selector.cpp`、`widgets/command_palette.cpp`。
- **兼容别名**：v7 破坏性重命名（`ScreenInteractive→App`、`Pixel→Cell`、
  `PixelAt→CellAt`）均提供兼容别名，现有 v5 风格代码无需改动。

## 故障排查：终端乱码

- 必须在真实控制台运行（Windows Terminal / conhost / mintty）。经管道、重定向或
  IDE 输出窗运行时，ANSI 序列会按字面显示，出现 TUI 消失 / `[555;35M` 等乱码。
- 仍乱码时先确认终端类型与 PowerShell 版本；必要时在 `Loop` 之前显式调用
  `screen.TrackMouse(false)` 关闭鼠标跟踪，避免 CSI 序列泄漏到屏幕。