# Ctrl+O 详情视图设计

## 目标

按下 Ctrl+O 切换"详情视图"，按时间顺序展示**当前一轮 Agent 编排**的全部内部信息：

1. **模型思考（Thought）**：reasoning + 正文 content + 持续秒数
2. **文件读取（Read）**：调用参数（path）+ 完整返回内容
3. **文件写入（Write）**：调用参数（path + content）+ 返回结果
4. **最终答案（FinalAnswer）**：完整正文 + reasoning

收起后回到对话流，保留原对话渲染。

---

## 现状分析

### Ctrl+O 已有链路
- 按键捕获：`platform_*` → `LineEditor` (`KEY_CTRL_O` = 0xE00A) → `Terminal::read_input` ([terminal.cpp:282-285](file:///d:/develop/Workspace/workx/src/tui/core/terminal.cpp#L282-L285))
- 回调注册：[main.cpp:300-303](file:///d:/develop/Workspace/workx/src/app/main.cpp#L300-L303) `terminal.set_ctrl_o_callback([&renderer]() { renderer.toggle_thinking_view(); })`
- 视图实现：[chat_renderer.cpp:367-414](file:///d:/develop/Workspace/workx/src/tui/render/chat_renderer.cpp#L367-L414) `toggle_thinking_view()`

### 当前 `toggle_thinking_view()` 的局限
1. **只展示 `m_reasoning_buffer`**（仅 reasoning_delta 累积），不包含：
   - Thought 正文（thought_text）
   - ToolCall 的参数（`ToolCallEvent::arguments` 字段当前未保存）
   - ToolResult 的完整内容（`ToolResultEvent::result` 当前未保存）
2. **只保留当前轮**：[chat_renderer.cpp:52](file:///d:/develop/Workspace/workx/src/tui/render/chat_renderer.cpp#L52) `m_reasoning_buffer.clear()` 在每次 `BackendStatusEvent::Connecting` 时清空，跨轮历史丢失
3. **无滚动支持**：内容多时只能看末尾，无法回看顶部
4. **无结构化展示**：reasoning 与正文、工具调用混杂，难以区分

### 用户提到的"回显"功能
- [terminal.cpp:198](file:///d:/develop/Workspace/workx/src/tui/core/terminal.cpp#L198) `echo_input(text)` 是用户回车后把输入文本回显到屏幕（灰底 + "> " 前缀）
- 这是输入回显，不是 Ctrl+O 详情视图的"回显"——本设计中的"显示/隐藏"指详情视图的展开/收起

---

## 数据结构设计

新增 `SessionLog` 类，按时间顺序累积本轮 Agent 编排的所有条目。

### 文件位置
- `src/tui/render/session_log.h`
- `src/tui/render/session_log.cpp`

### 条目结构

```cpp
// session_log.h
#pragma once
#include <string>
#include <vector>
#include <chrono>

namespace agent {

enum class LogEntryType {
    Thought,        // Thought 阶段（含中间轮 + 最终答案轮）
    ToolCall,       // 工具调用
    ToolResult,     // 工具返回
    FinalAnswer,    // 最终答案（无 tool_use 的 Thought）
};

struct LogEntry {
    LogEntryType type;
    int32_t step_number = 0;             // ReAct 步骤号

    // Thought / FinalAnswer
    std::string reasoning;               // 推理内容
    std::string content;                 // 正文内容
    int32_t thinking_seconds = 0;        // 思考持续秒数

    // ToolCall
    std::string tool_name;               // 工具名（Read/Write/...）
    std::string arguments_json;          // 工具参数（JSON 字符串）

    // ToolResult
    std::string result;                  // 工具返回内容
    bool is_error = false;
};

class SessionLog {
public:
    void clear();                        // 新一轮 Agent 编排开始时清空
    void add_thought(int32_t step, std::string reasoning, std::string content, int32_t seconds);
    void add_final_answer(int32_t step, std::string reasoning, std::string content, int32_t seconds);
    void add_tool_call(int32_t step, std::string tool_name, std::string arguments_json);
    void add_tool_result(int32_t step, std::string result, bool is_error);

    const std::vector<LogEntry>& entries() const { return m_entries; }
    bool empty() const { return m_entries.empty(); }

private:
    std::vector<LogEntry> m_entries;
};

} // namespace agent
```

### 何时清空
- 在 `BackendStatusEvent::Connecting` 时 clear（与现有 `m_reasoning_buffer.clear()` 同步）
- 这样每轮用户输入对应一份完整 log

---

## 视图渲染设计

### 整体布局

清屏后采用**三段式布局**：顶部标题行（固定，单行）+ 中间滚动区（无边框卡片列表）+ 底部状态行（固定，单行）。

**关键变更**（参考 Claude Code transcript 模式与 FileReadTool/FileWriteTool UI 配色，见 [cc.md](file:///d:/develop/Workspace/workx/.claude/plan/cc.md) 与 [cc-code.md](file:///d:/develop/Workspace/workx/.claude/plan/cc-code.md)）：
- **去掉所有边框**：卡片无外框，子区块无 `┌─ ┐` 框，无左侧 `┃` 竖线
- **消息标记 `⏺`**：每个条目以 `⏺`（实心圆，Claude 橙）开头，对齐 Claude Code 的 `<AssistantToolUseMessage>` 风格
- **去掉步骤编号**：不再显示"步骤 N"，靠消息流自然顺序（`LogEntry.step_number` 仍保留在数据层，仅不在 UI 显示）
- **中文类型名**：思考/读取/写入/最终答复（Read/Write 工具后跟双空格 + 文件路径，如 `⏺ 读取  react_test.txt`）
- **代码区统一格式**：`<行号>  <代码>`（行号右对齐 + 双空格 + 代码），不再使用 `N→内容` 箭头形式
- **diff 暗色背景**：使用 Claude Code 的 `diffAdded` `rgb(34,92,43)` / `diffRemoved` `rgb(122,41,54)` 暗色 RGB 背景，不用亮绿/亮红
- **结果前缀 `⎿`**：所有工具结果行前加 `⎿  `（dimColor 浅灰），与 Claude Code `<MessageResponse>` 一致
- **不显示 diff 头符号**：过滤掉 `+++`/`---`/`@@` 行，只保留内容行

```
详情视图 · Ctrl+O 返回
─────────────────────────────────────────────────────────────────────

  ⏺ 思考 · 8s
    推理：用户想读取 react_test.txt 的内容，需要调用 Read 工具
    正文：（无，已调用工具）

  ⏺ 读取  react_test.txt
    ⎿  Read 1 line
       1  Hello ReAct Loop

  ⏺ 写入  output.txt
    ⎿  Updated output.txt
       1  旧的内容                            ← 无背景（上下文）
      -2  旧的第二行                          ← 暗红背景 rgb(122,41,54)
      +2  文件内容是：                        ← 暗绿背景 rgb(34,92,43)
      +3  Hello ReAct Loop                    ← 暗绿背景 rgb(34,92,43)

  ⏺ 最终答复
    推理：已读取到内容，可以直接回答用户
    正文：
    文件内容是：
    Hello ReAct Loop

─────────────────────────────────────────────────────────────────────
↑↓ / j·k 滚动 · g/G 顶底 · q 或 Ctrl+O 返回 · 35%
```

### 卡片结构（无边框）

每个条目一张卡片，由三部分组成（无任何边框、无左侧竖线）：

1. **首行：`⏺` 消息标记 + 中文类型名 + 附加信息**（`⏺` 用对应步骤类型的颜色，见"颜色方案"）
   - 思考：`⏺ 思考 · 8s`（附加思考秒数）
   - 读取：`⏺ 读取  react_test.txt`（类型 + 双空格 + 文件路径）
   - 写入：`⏺ 写入  output.txt`（类型 + 双空格 + 文件路径）
   - 最终答复：`⏺ 最终答复`（无附加信息）
2. **内容行**（每行前加 4 空格缩进，对齐 `⏺ ` 之后的内容）：
   - 思考：`推理：...`（可选）+ `正文：...`（可选，若为空显示"（无，已调用工具）"）
   - 读取：`⎿  Read N lines`（前缀 `⎿  ` 浅灰 + "Read N lines" 白色 + 加粗 N）+ 代码区
   - 写入（create）：`⎿  Wrote N lines to <path>`（加粗 N 和 path）+ 代码区
   - 写入（update）：`⎿  Updated <path>` + diff 代码区
   - 最终答复：`推理：...`（可选）+ `正文：...`
3. **空行分隔**：卡片之间空一行

**缩进对齐规则**（参考 Claude Code `⏺` / `⎿` 层级）：
- `⏺` 位于列 2（前导 2 空格）
- 内容行位于列 4（前导 4 空格，对齐 `⏺ ` 之后）
- `⎿` 位于列 4（与内容行同列）
- 代码区位于列 7（前导 7 空格，对齐 `⎿  ` 之后的内容）

### 代码区格式（行号 + 代码）

参考 Claude Code 的 `<HighlightedCode>` 渲染（[cc-code.md](file:///d:/develop/Workspace/workx/.claude/plan/cc-code.md)），所有代码统一采用 **行号 + 双空格 + 代码** 格式：

```
   1  import { foo } from 'bar'
   2  export function baz() {
   3    return foo()
   4  }
```

- **行号右对齐**：宽度按最大行号位数计算（如最多 3 位则 `  1`、` 12`、`123`）
- **行号颜色**：`dimColor` 浅灰 `rgb(153,153,153)`
- **分隔符**：行号与代码之间为 **2 个空格**（不用 `→` 箭头）
- **代码颜色**：默认白色 `rgb(255,255,255)`；如支持语法高亮则按文件扩展名染色
- **缩进**：每行前加 7 空格，对齐 `⎿  ` 之后的内容（见"卡片结构"缩进规则）
- **无背景色**：普通代码行透明背景，仅 diff 行才有背景色

### Git Diff 着色方案（核心）

`FileWriteTool` 在 update 模式下已经返回 unified diff 格式（[file_write_tool.cpp:351-363](file:///d:/develop/Workspace/workx/src/agent/tool/FileWriteTool/file_write_tool.cpp#L351-L363) 调用 [diff.h:format_diff](file:///d:/develop/Workspace/workx/src/agent/tool/FileWriteTool/diff.h#L62-L65)）。详情视图中渲染 Write 内容时，参考 Claude Code 的 `<FileEditToolUpdatedMessage>` 配色（[cc-code.md](file:///d:/develop/Workspace/workx/.claude/plan/cc-code.md)）：

**第一步：过滤 diff 头**
- 跳过以 `---`、`+++`、`@@` 开头的行（不显示这些符号）
- 保留 `+`/`-`/` `（空格）开头的内容行

**第二步：标记 + 行号 + 双空格 + 代码 + 背景色**

| 原前缀 | 渲染格式 | 背景色 | ANSI 序列（TrueColor） | 行号来源 |
|--------|---------|--------|----------------------|---------|
| `+`（新增） | `+<行号>  <代码>` | 暗绿 `rgb(34,92,43)` | `\x1b[48;2;34;92;43m` | new_line |
| `-`（删除） | `-<行号>  <代码>` | 暗红 `rgb(122,41,54)` | `\x1b[48;2;122;41;54m` | old_line |
| ` `（上下文） | ` <行号>  <代码>` | 无背景 | — | new_line |

**第三步：行号计算**
- 从 `@@ -old_start,old_len +new_start,new_len @@` 头部解析起始行号（该行不显示，但行号信息需要提取）
- 维护两个计数器：`old_line`（从 `old_start` 开始）、`new_line`（从 `new_start` 开始）
- `+` 行：使用 `new_line`，然后 `new_line++`
- `-` 行：使用 `old_line`，然后 `old_line++`
- ` `（空格）行：使用 `new_line`（显示用），`old_line++`、`new_line++`

**第四步：标记 + 行号对齐**
- 行号宽度按本 diff 块内最大行号位数计算（如最多 3 位则 `  1`、` 12`、`123`）
- 标记符 `+`/`-`/` ` 始终占 1 字符，位于行号之前
- 渲染示例：` + 12  code` / ` -  3  code` / `    1  code`

**字级高亮（可选增强）**：
- 新增字前景色：`diffAddedWord` `rgb(56,166,96)` —— `\x1b[38;2;56;166;96m`
- 删除字前景色：`diffRemovedWord` `rgb(179,89,107)` —— `\x1b[38;2;179;89;107m`
- 实现时可在 `+`/`-` 行内对变更字加前景色（与背景叠加）

**实现要点**：
- 每行末尾追加 `\x1b[0m` 重置
- create 模式（无 diff，只有 "File created successfully"）：标题用 `⎿  Wrote N lines to <path>`，代码区按"代码区格式"渲染（无 `+`/`-` 标记，无背景色，纯 `行号 + 双空格 + 代码`）
- 错误结果：`⎿  Error writing file` 或 `⎿  File not found`，用 `error` 亮红 `rgb(255,107,128)` 渲染
- **不使用亮绿/亮红背景**（`\x1b[42;30m` / `\x1b[41;30m` / `\x1b[100;30m`），改用暗色 TrueColor 背景，更接近 Claude Code 视觉风格
- 行号右对齐宽度按最大行号位数计算（同 [file_read_tool.cpp:138-156](file:///d:/develop/Workspace/workx/src/agent/tool/FileReadTool/file_read_tool.cpp#L138-L156) 的 `format_with_line_numbers` 逻辑）

### Read 工具结果渲染

`FileReadTool::call()` 返回带行号的内容（[file_read_tool.cpp:130-162](file:///d:/develop/Workspace/workx/src/agent/tool/FileReadTool/file_read_tool.cpp#L130-L162) `format_with_line_numbers`）。详情视图中参考 Claude Code 的 FileReadTool UI（[cc-code.md](file:///d:/develop/Workspace/workx/.claude/plan/cc-code.md)）：

**结构**：
```
  ┃ 步骤 2 · 读取 · react_test.txt
  ┃   ⎿  Read 1 line
  ┃      1  Hello ReAct Loop
```

- **结果标题**：`⎿  Read N lines`（N 加粗 `bold`）
  - 前缀 `⎿  ` 用 `dimColor` 浅灰 `rgb(153,153,153)`
  - 正文 "Read N lines" 用白色，N 加粗
  - 单行用 "line"，多行用 "lines"
- **代码区**：按"代码区格式"渲染（`<行号>  <代码>`）
  - **无背景色**：默认透明背景（不再像原设计那样整段灰背景），让代码本身更突出
  - 不做 diff 着色（Read 是只读操作，无变更概念）
  - 如支持语法高亮，按文件扩展名染色
- **特殊结果**：
  - 文件未修改（缓存命中）：`⎿  Unchanged since last read`，整行 `dimColor` 浅灰
  - 文件不存在：`⎿  File not found`，用 `error` 亮红 `rgb(255,107,128)`
  - 读取错误：`⎿  Error reading file`，用 `error` 亮红
  - 图片/PDF/notebook：`⎿  Read image (245 KB)` / `⎿  Read PDF (2.3 MB)` / `⎿  Read 12 cells`（cells 数加粗）

### 颜色方案（`⏺` 标记 + 标题 + 文本）

参考 Claude Code dark theme 配色（[cc.md "配色方案"](file:///d:/develop/Workspace/workx/.claude/plan/cc.md)）：

| 元素 | 颜色 | RGB | ColorRole 对应 | 用途 |
|------|------|-----|---------------|------|
| 思考 `⏺`/标题 | Claude 橙 | `rgb(215,119,87)` | `claude` | `⏺ 思考`、reasoning 行 |
| 读取 `⏺`/标题 | 浅蓝紫 | `rgb(177,185,249)` | `permission` | `⏺ 读取`、文件名 |
| 写入 `⏺`/标题 | 琥珀 | `rgb(255,193,7)` | `warning` | `⏺ 写入`、文件名 |
| 最终答复 `⏺`/标题 | 亮绿 | `rgb(78,186,101)` | `success` | `⏺ 最终答复`、正文 |
| 错误 | 亮红 | `rgb(255,107,128)` | `error` | 错误消息 |
| dimColor | 浅灰 | `rgb(153,153,153)` | `inactive` | `⎿` 前缀、行号、状态栏、提示 |
| 正文 | 白 | `rgb(255,255,255)` | `text` | 默认文本 |
| diffAdded 背景 | 暗绿 | `rgb(34,92,43)` | `diffAdded` | `+` 行整行背景 |
| diffRemoved 背景 | 暗红 | `rgb(122,41,54)` | `diffRemoved` | `-` 行整行背景 |
| diffAddedWord 前景 | 中绿 | `rgb(56,166,96)` | `diffAddedWord` | `+` 行内变更字 |
| diffRemovedWord 前景 | 柔红 | `rgb(179,89,107)` | `diffRemovedWord` | `-` 行内变更字 |

内容区前缀（4 空格缩进）用对应步骤类型的颜色（淡化为 50% 亮度，避免抢眼）。`⏺` 标记本身用对应步骤类型的颜色（满亮度），作为视觉锚点。

### 顶部/底部固定行

**顶部标题行**（单行，Claude 橙 `rgb(215,119,87)`）：
```
详情视图 · 共 N 步 · Ctrl+O 返回
```
下方接一条 `─` 分隔线（横跨终端宽度，`dimColor` 浅灰）。

**底部状态行**（单行，`dimColor` 浅灰 `rgb(153,153,153)`）：
```
↑↓ / j·k 滚动 · g/G 顶底 · q 或 Ctrl+O 返回 · NN%
```
上方接一条 `─` 分隔线（`dimColor` 浅灰）。百分比 = 当前滚动位置 / 总行数。

### 收起视图（再按 Ctrl+O）

清屏后重新渲染对话流摘要：
- `● Thought for Ns (ctrl+o to view)` 标记（已有逻辑）
- 本轮所有 ToolCall 摘要行（`◃ Read react_test.txt` 等，已有逻辑）
- 本轮 FinalAnswer 内容（已有流式渲染）
- 光标复位到输入行

这部分逻辑已有（[chat_renderer.cpp:404-414](file:///d:/develop/Workspace/workx/src/tui/render/chat_renderer.cpp#L404-L414)），但当前只重渲最后一轮标记。需要扩展为重渲**整轮对话流**（Thought 标记 + 所有 ToolCall 摘要 + FinalAnswer 内容）。

---

## 滚动交互设计

### 问题
当前 `LineEditor::read_input` 只在等待用户输入时捕获按键，且只识别 Ctrl+O。详情视图展开后需要捕获滚动键。

### 方案
在 `Terminal::read_input` 主循环中，当 `m_viewing_detail`（新标志）为 true 时，进入"查看模式"循环：
- 不调用 `LineEditor::read_input`
- 直接读取按键，识别：
  - `↑` / `k`：向上滚动一行
  - `↓` / `j`：向下滚动一行
  - `PageUp` / `Ctrl+B`：向上翻页
  - `PageDown` / `Ctrl+F`：向下翻页
  - `g`：跳到顶部
  - `G`：跳到底部
  - `q` / `Esc` / `Ctrl+O`：退出查看模式
- 每次滚动后重新渲染可见区域

### 渲染策略
- 计算所有条目的总行数 `total_lines`
- 视口高度 = `terminal_height - 3`（留出顶部边框 + 底部状态行 + 输入行）
- 维护 `m_scroll_offset`（顶部偏移行数）
- 渲染时只输出 `[offset, offset + viewport_height)` 范围内的行
- 顶部固定一行标题：`  Detail View · Step 1-N · ↑↓ scroll · Ctrl+O return`
- 底部固定一行：滚动进度条 + 提示

### 实现位置
- 在 `ChatRenderer` 中新增 `render_detail_view(int scroll_offset)` 方法
- `Terminal::read_input` 中新增分支：当 `m_detail_view_active` 为 true 时，调用 `ChatRenderer::handle_detail_input(key)` 而不是 `LineEditor::read_input`

---

## 改动点清单

### 1. 新增文件
- `src/tui/render/session_log.h` —— `LogEntry` + `SessionLog` 类声明
- `src/tui/render/session_log.cpp` —— 实现

### 2. 修改 `src/tui/render/chat_renderer.h`
- 新增成员：`SessionLog m_session_log`
- 新增成员：`bool m_detail_view_active = false`、`int m_scroll_offset = 0`
- 重命名方法：`toggle_thinking_view()` → `toggle_detail_view()`（保留旧名为别名，避免破坏 main.cpp 调用）
- 新增方法：`void handle_detail_input(char32_t key)`、`void render_detail_view()`

### 3. 修改 `src/tui/render/chat_renderer.cpp`
- `BackendStatusEvent::Connecting` handler 中追加 `m_session_log.clear()`
- `StreamTokenEvent` handler 中：把 reasoning_delta 累积到一个临时缓冲（`m_current_thought_reasoning`），把 content_delta 累积到 `m_current_thought_content`，供 Thought 完成时存入 log
- `AgentStepEvent` handler（Thought 分支）：调用 `m_session_log.add_thought(...)`，把累积的 reasoning + content 存入 log
- `ToolCallEvent` handler：调用 `m_session_log.add_tool_call(step, tool_name, arguments)`
- `ToolResultEvent` handler：调用 `m_session_log.add_tool_result(step, result, is_error)`
- `StreamDoneEvent` handler（最终轮）：调用 `m_session_log.add_final_answer(...)`（如果 full_content 非空）
- 重写 `toggle_thinking_view()`：调用 `toggle_detail_view()`
- 新增 `toggle_detail_view()`：
  - 展开时：`m_detail_view_active = true`，`m_scroll_offset = 0`，调用 `render_detail_view()`
  - 收起时：`m_detail_view_active = false`，清屏 + 重新渲染对话流
- 新增 `render_detail_view()`：按上节"视图渲染设计"实现，支持 scroll_offset
- 新增 `handle_detail_input(char32_t key)`：按上节"滚动交互设计"实现

### 4. 修改 `src/tui/core/terminal.h` / `.cpp`
- 新增成员：`bool m_detail_view_active = false`（与 ChatRenderer 同步，或者通过查询接口）
- `read_input` 主循环中：如果 `m_detail_view_active`，进入查看模式分支
  - 不调用 `LineEditor::read_input`
  - 直接读按键，调用 `m_ctrl_o_callback`（如果按 Ctrl+O）或 `m_detail_input_callback`（其他键）
- 新增回调类型：`using DetailInputCallback = std::function<void(char32_t)>;`
- 新增 setter：`void set_detail_input_callback(DetailInputCallback cb);`

### 5. 修改 `src/app/main.cpp`
- 在 Ctrl+O 回调注册处，增加 DetailInputCallback 注册：
  ```cpp
  terminal.set_detail_input_callback([&renderer](char32_t key) {
      renderer.handle_detail_input(key);
  });
  ```
- ChatRenderer 需要 expose `m_detail_view_active` 给 Terminal 查询（或通过 `state()` 扩展 TuiState::DETAIL_VIEW）

### 6. 修改 `src/tui/core/tui_state.h`
- 新增状态：`DETAIL_VIEW`（与 THINKING/STREAMING/TOOL_RUNNING/IDLE 等并列）
- 状态机允许 `DETAIL_VIEW ↔ IDLE` 转换

### 7. CMakeLists.txt
- 把 `session_log.cpp` 加入 libworkx 源文件列表

---

## 边界情况

1. **详情视图展开期间，后台 Agent 仍在工作（流式 token 还在来）**
   - 当 `m_detail_view_active == true` 时，StreamTokenEvent handler 不再写入终端，但仍累积到 log
   - 这样用户在查看时，新内容静默进入 log，退出查看后能看到

2. **详情视图展开期间，用户按 Ctrl+C**
   - 视为退出查看模式（不触发中断），与 `q` / `Esc` 等价
   - 如果用户在查看模式下连按两次 Ctrl+C，才触发中断

3. **空 log 时按 Ctrl+O**
   - 如果 `m_session_log.empty()`，不进入查看模式，可选终端闪烁或显示 "No detail available"

4. **终端 resize**
   - 详情视图需要重新计算 viewport_height 并重绘
   - 复用现有的 SIGWINCH/resize 处理钩子

5. **超大 log（100+ 条目）**
   - 渲染时只渲染可见区域，不要一次性输出所有条目
   - 滚动时增量渲染（或简单清屏 + 重绘可见区域）

---

## 实现顺序建议

1. **先建数据层**：`SessionLog` + `LogEntry`，单元测试覆盖 add/clear
2. **在事件 handler 中追加 log**：先不动 UI，验证 log 数据正确
3. **重写 `toggle_detail_view()` 展开逻辑**：先不支持滚动，一次输出全部
4. **加滚动支持**：扩展 Terminal 按键处理 + `handle_detail_input`
5. **处理边界情况**：流式静默、Ctrl+C 退出、resize

每步独立可验证：
- 步骤 1 验证：单元测试 `SessionLog` 各方法
- 步骤 2 验证：在 `toggle_thinking_view()` 临时打印 `m_session_log.entries().size()` 确认数据进入
- 步骤 3 验证：按 Ctrl+O 看到结构化列表
- 步骤 4 验证：滚动按键工作
- 步骤 5 验证：各边界场景手测

---

## 不做的事（明确排除）

- 不做跨轮历史查看（只看当前这一轮 Agent 编排，下一轮自动清空）
- 不做搜索/过滤（如 grep log 内容）
- 不做导出/保存 log 到文件
- 不做 vim 风格的 ex 命令（`:q` 等）
- 不改 LineEditor 现有按键处理（只在 Terminal 层新增查看模式分支）
