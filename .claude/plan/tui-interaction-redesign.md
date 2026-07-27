# TUI 交互与动画重设计方案

## 一、TUI 状态机

### 1.1 状态定义

```
┌─────────────────────────────────────────────────────┐
│                   TUI 状态机                         │
│                                                     │
│  ┌──────────┐   user_input    ┌───────────┐        │
│  │  IDLE    │───────────────→ │ THINKING  │        │
│  │ (等待输入)│ ←────────────── │ (思考中)   │        │
│  └──────────┘   interrupt     └─────┬─────┘        │
│       ↑          error              │ first_token   │
│       │                             ↓              │
│       │  ┌───────────┐    ┌──────────────┐        │
│       │  │  ERROR    │    │  STREAMING   │        │
│       │  │ (错误显示) │    │  (流式输出)   │        │
│       │  └───────────┘    └──────┬───────┘        │
│       │                          │                 │
│       │     ┌──────────────┐     │ stream_done     │
│       └─────│  TOOL_RUNNING │←───┘                 │
│             │  (工具执行)   │───→ tool_result       │
│             └──────┬───────┘     │                  │
│                    │             ↓                  │
│                    │       ┌───────────┐            │
│                    └──────→│   IDLE    │            │
│                            └───────────┘            │
└─────────────────────────────────────────────────────┘
```

### 1.2 状态与事件映射

| 当前状态 | 触发事件 | 目标状态 | TUI 动作 |
|---|---|---|---|
| IDLE | `UserInputEvent` | THINKING | 清输入行, 显示思考指示器 |
| IDLE | `CommandEvent` | IDLE | 执行命令, 显示结果 |
| THINKING | `StreamTokenEvent`(首token, content) | STREAMING | 折叠思考块, 开始流式渲染 |
| THINKING | `StreamTokenEvent`(reasoning) | THINKING | 更新思考计时, 追加推理文本 |
| THINKING | `StreamErrorEvent` | ERROR | 停止思考动画, 显示错误 |
| THINKING | `InterruptEvent` | IDLE | 停止思考动画, 恢复提示符 |
| STREAMING | `StreamTokenEvent`(content) | STREAMING | 追加流式文本, 更新token计数 |
| STREAMING | `StreamTokenEvent`(reasoning) | STREAMING | 切换到推理块渲染 |
| STREAMING | `StreamDoneEvent` | IDLE | 显示token统计, 恢复提示符 |
| STREAMING | `ToolCallEvent` | TOOL_RUNNING | 显示工具调用, 开始工具动画 |
| STREAMING | `StreamErrorEvent` | ERROR | 显示错误信息 |
| TOOL_RUNNING | `ToolResultEvent` | STREAMING | 显示工具结果, 继续流式 |
| TOOL_RUNNING | `StreamDoneEvent` | IDLE | 显示完成统计 |
| ERROR | (any input) | IDLE | 清除错误, 恢复提示符 |

### 1.3 状态枚举实现

```cpp
enum class TuiState {
    IDLE,           // 等待用户输入
    THINKING,       // 模型思考中（显示推理块 + 计时器）
    STREAMING,      // 流式输出内容
    TOOL_RUNNING,   // 工具调用执行中
    ERROR           // 错误状态
};
```

### 1.4 屏幕布局分区

重设计后, 终端屏幕分为 3 个逻辑区域:

```
┌──────────────────────────────────────────────┐
│                                              │
│ [Output Area]  主输出区域 (滚动)               │
│   - 欢迎信息                                  │
│   - 对话内容                                  │
│   - 工具调用/结果                              │
│   - 错误信息                                  │
│                                              │
├──────────────────────────────────────────────┤
│ ❯ [Input Line]  输入行 (LineEditor)           │
├──────────────────────────────────────────────┤
│ [Status Bar]  底部状态栏 (1行)                 │
└──────────────────────────────────────────────┘
```

**关键变化**: Spinner 不再在行末显示, 而是拥有独立的 Status Bar 区域, 与输出和输入完全隔离。Status Bar 位于输入行下方（屏幕最底部），输入行始终在 Status Bar 上方。

---

## 二、Splash Screen 设计

### 2.1 ASCII Art Logo

```
 ╦╔═╔═╗╔╗╔╔═╗╦═╗
 ╠╩╗║ ║║║║║╣ ╠╦╝
 ╩ ╩╚═╝╝╚╝╚═╝╩╚═
```

使用 Unicode block-drawing 字符的替代版本（更好的终端兼容性）:

```
 ██████╗ ██████╗ ███████╗
 ██╔══██╗██╔══██╗██╔════╝
 ██║  ██║██████╔╝███████╗
 ██║  ██║██╔══██╗╚════██║
 ██████╔╝██║  ██║███████║
 ╚═════╝ ╚═╝  ╚═╝╚══════╝
```

### 2.2 完整 Splash Screen

```
 ██████╗ ██████╗ ███████╗    Workx v0.2.0
 ██╔══██╗██╔══██╗██╔════╝    glm-5.1 · API Usage Billing
 ██║  ██║██████╔╝███████╗    D:\develop\Workspace\workx
 ██║  ██║██╔══██╗╚════██║
 ██████╔╝██║  ██║███████║    /help for commands
 ╚═════╝ ╚═╝  ╚═╝╚══════╝
```

### 2.3 简化版（窄终端 fallback，< 60 列）

```
 [Workx] v0.2.0
 glm-5.1
 /help for commands
```

### 2.4 Splash 渲染策略

- 检测终端宽度, > 60 列使用完整 ASCII art, 否则使用简化版
- ASCII art 使用 `ColorRole::Assistant` (青色) 渲染
- 右侧信息使用 `ColorRole::System` (紫色) 渲染
- Logo 和信息之间使用空格对齐（不使用 tab, 兼容性更好）
- Logo 字符应可配置, 通过 `ConfigManager` 读取

---

## 三、Status Bar 设计

### 3.1 状态栏内容

```
[glm-5.1] │ workx │ ⏱ 1m23s │ 83% ctx │ 1.2k tokens
```

各段含义:
- `[glm-5.1]` — 当前模型名 (蓝色)
- `workx` — 项目名/工作目录名 (灰色)
- `⏱ 1m23s` — 会话持续时间 (默认色)
- `83% ctx` — 上下文使用率 (使用率 > 90% 时变红色)
- `1.2k tokens` — 本轮已生成 token 数 (默认色)

### 3.2 状态栏按 TUI 状态变化

**IDLE 状态:**
```
[glm-5.1] │ workx │ ⏱ 1m23s │ idle
```

**THINKING 状态:**
```
[glm-5.1] │ workx │ ⏱ 1m23s │ ● thinking... 4s │ 0 tokens
```
其中 `●` 是闪烁的思考动画字符（见动画序列部分），思考中闪烁，完成后变为静态绿色 `●`，失败变为红色 `●`。

**STREAMING 状态:**
```
[glm-5.1] │ workx │ ⏱ 1m23s │ ⟡ streaming │ 1.2k tokens
```

**TOOL_RUNNING 状态:**
```
[glm-5.1] │ workx │ ⏱ 1m23s │ ⚙ readFile... │ 1.2k tokens
```

**ERROR 状态:**
```
[glm-5.1] │ workx │ ⏱ 1m23s │ ✗ error │ 1.2k tokens
```

### 3.3 Status Bar 渲染机制

```
位置: 最后一行（输入行下方，屏幕最底部）
渲染: \x1b[s 保存光标 → \x1b[?25l 隐藏光标 → 移动到状态栏行 → 写入 → \x1b[u 恢复光标 → \x1b[?25h 显示光标
分隔符: \x1b[2K 清整行 → 重写（避免残留字符）
颜色: 背景使用 reversed video (\x1b[7m) 或 237 灰色背景 (\x1b[48;5;237m)
```

---

## 四、Thinking/Reasoning 显示

### 4.1 折叠态（默认）

**思考中（THINKING 状态）:**
```
 ● Thinking... 4s (ctrl+o to expand)
```
- `●` 闪烁动画（见动画序列部分）
- `Thinking... Ns` — 思考持续时间, 实时更新
- `(ctrl+o to expand)` — 操作提示, 灰色

**思考完成:**
```
 ● Thought for 4s (ctrl+o to expand)
```
- `●` 静态绿色（表示成功完成思考）

**思考失败:**
```
 ● Thought for 4s (ctrl+o to expand)
```
- `●` 静态红色（表示思考过程出错）

### 4.2 展开态

```
 ┌─ ● Thought for 4s (ctrl+o to collapse) ─────────┐
 │ To solve this problem, I need to first analyze  │
 │ the user's request and then determine the best  │
 │ approach for implementation...                   │
 └─────────────────────────────────────────────────┘
```

- 展开态标题行中 `●` 同样根据状态显示颜色：思考中闪烁，完成绿色，失败红色

### 4.3 思考块状态流转

```
THINKING 开始:
  → 输出 " ● Thinking..." (●闪烁动画开始)
  → 每收到 reasoning_delta, 追加到内部缓冲区
  → 每秒更新计时 "Thinking... Ns"

首 content token 到达:
  → ● 闪烁动画停止
  → 将思考块折叠为 "● Thought for Ns (ctrl+o to expand)"，●变为静态绿色
  → 输出空行, 开始流式内容输出

思考出错:
  → ● 闪烁动画停止
  → 将思考块折叠为 "● Thought for Ns (ctrl+o to expand)"，●变为静态红色
```

### 4.4 ctrl+o 交互

- 在 IDLE 状态时, ctrl+o 切换最近一个思考块的展开/折叠
- 展开时, 在原位置替换显示
- 折叠时, 恢复为单行
- 需要在 `LineEditor` 中注册 ctrl+o 为特殊按键

### 4.5 多思考块场景

```
 ● Thought for 4s (ctrl+o to expand)

 Assistant response content here...

 ● Thought for 2s (ctrl+o to expand)

 Second response content...
```

每个思考块独立折叠/展开, 使用索引标记。`●` 根据各自状态显示颜色（绿色=完成，红色=失败）。

---

## 五、Streaming Output 格式化

### 5.1 基本文本流

```
 Here is the implementation of the function:
```

- 助手内容使用 `ColorRole::Assistant` (青色)
- 逐 token 输出, 无额外装饰
- 保持当前的逐 token 写入方式（优化见性能部分）

### 5.2 结构化输出 — 列表

```
 Here are the steps:
 • First, read the configuration file
 • Then, parse the user input
 • Finally, execute the command
```

- 检测 `- ` / `* ` 开头的行, 替换为 `• ` (Unicode bullet)
- 后续行缩进对齐

### 5.3 代码块渲染

```
 ┌─ main.cpp ─────────────────────────────────┐
 │ #include <iostream>                        │
 │                                            │
 │ int main() {                               │
 │     std::cout << "Hello" << std::endl;     │
 │ }                                          │
 └────────────────────────────────────────────┘
```

- 检测 ``` ``` ``` 围栏, 替换为 box-drawing 边框
- 文件名在顶部标题行（若有语言标识符则显示）
- 每行代码带 `│ ` 前缀
- 背景色: `\x1b[48;5;236m` (深灰), 区分普通文本

### 5.4 流式输出时的缓冲策略

当前问题: 每个 token 一次 `write()` + `m_output_mutex` 锁。

**优化方案: Token Buffer**

```cpp
class StreamingBuffer {
    std::string m_buffer;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_flush_thread;
    static constexpr auto FLUSH_INTERVAL = 16ms; // ~60fps

    void flush_loop() {
        while (m_running) {
            std::unique_lock lock(m_mutex);
            m_cv.wait_for(lock, FLUSH_INTERVAL, [&]{ return !m_buffer.empty(); });
            if (!m_buffer.empty()) {
                std::string chunk = std::move(m_buffer);
                m_buffer.clear();
                lock.unlock();
                m_terminal.write_safe(chunk);  // 单次锁获取 + 写入
            }
        }
    }
};
```

- Token 写入先进入缓冲区（无锁快速路径用 spinlock）
- 刷新线程每 16ms 批量写入一次
- 停止流式时立即 flush 剩余内容

---

## 六、Tool Call 可视化

### 6.1 工具调用开始

```
 ⚙ ReadFile("src/main.cpp")
   ⋮ reading...
```

- `⚙` 工具图标 (或用 `▶` 替代, 根据终端兼容性)
- 工具名用 `ColorRole::ToolName` (蓝色)
- 参数预览用默认色, 截断到终端宽度 - 20
- `⋮ reading...` 旋转等待指示

### 6.2 工具调用完成 — 成功

```
 ⚙ ReadFile("src/main.cpp") ✓
   ⎿  42 lines · 1.2KB
```

- `✓` 成功标记 (绿色)
- `⎿` 嵌套结果指示符
- 结果预览截断到 1 行

### 6.3 工具调用完成 — 失败

```
 ⚙ ReadFile("nonexistent.cpp") ✗
   ⎿  File not found
```

- `✗` 失败标记 (红色)
- 错误信息用 `ColorRole::Error`

### 6.4 嵌套工具调用

```
 ⚙ Agent("refactor")
   ⚙ ReadFile("src/parser.h")
     ⎿  156 lines · 4.1KB
   ⚙ EditFile("src/parser.h")
     ⎿  Applied 3 changes
   ✓ Agent complete: 3 tool calls, 12s
```

- 每层嵌套增加 2 空格缩进
- 外层 Agent 未完成时, 内层工具逐个显示
- Agent 完成时显示汇总行

### 6.5 工具类型图标映射

```cpp
// 可配置的工具图标映射
static const std::unordered_map<std::string, std::string> TOOL_ICONS = {
    {"ReadFile",    "📖" /* 或 "▶" */},
    {"WriteFile",   "✏️" /* 或 "◀" */},
    {"EditFile",    "✂️" /* 或 "◆" */},
    {"Execute",     "⚡" /* 或 "►" */},
    {"Search",      "🔍" /* 或 "◈" */},
    {"Agent",       "⚙"  /* 或 "●" */},
    {"_default",    "▶"},
};
```

**注意**: 用户提到"emoji 要更换", 所以所有 emoji 应设计为可替换的。提供 Unicode fallback 图标用于不支持 emoji 的终端。运行时通过 `IPlatform` 能力检测选择图标集。

---

## 七、Animation Sequences

### 7.1 思考动画（替换当前 Spinner）

**当前**: `|/-\` 旋转, 100ms 间隔

**新设计**: ● 闪烁动画, 极简风格

```
思考中:   ● → (隐) → ● → (隐) → ...
           青色     空白    青色     空白

思考完成: ●  (静态, 绿色 \x1b[32m●\x1b[0m)

思考失败: ●  (静态, 红色 \x1b[31m●\x1b[0m)
```

- 闪烁通过交替显示/隐藏 `●` 实现（或交替切换前景色与背景色）
- 间隔: 500ms（慢速闪烁, 比 200ms 旋转更优雅, 不干扰阅读）
- 完成时 `●` 变为静态绿色, 失败时变为静态红色
- 状态栏和输出区的 `●` 同步闪烁, 由同一个计时器驱动

### 7.2 启动动画序列

```
T=0ms:    清屏
T=0ms:    显示 ASCII art logo (逐行, 每行 30ms 延迟)
T=180ms:  显示右侧信息 (模型名, 路径)
T=250ms:  显示 /help 提示
T=300ms:  显示状态栏
T=300ms:  显示输入提示符, 进入 IDLE
```

简化实现: 不需要逐行动画延迟, 一次性渲染全部内容即可。延迟动画在远程/慢终端上体验差。

### 7.3 状态转换动画

**IDLE → THINKING:**
```
1. 保存输入行内容到历史
2. 输入行清空
3. 状态栏更新为 "● thinking..."
4. 在输出区显示空行 + " ● Thinking..."
5. 开始思考计时器 (在 Status Bar 更新)
6. ● 开始闪烁动画
```

**THINKING → STREAMING:**
```
1. ● 闪烁动画停止
2. 将 " ● Thinking..." 替换为折叠态 "● Thought for Ns (ctrl+o to expand)"，●变为静态绿色
   技巧: \x1b[1A 上移一行 → \x1b[2K 清行 → 重写
3. 输出空行
4. 状态栏更新为 "⟡ streaming"
5. 开始流式内容输出
```

**STREAMING → IDLE (完成):**
```
1. 输出换行
2. 显示 token 统计行:
   "1,234 tokens · 45.6 tok/s · 12.3s"
   颜色: ColorRole::System (紫色)
3. 状态栏更新为 "idle"
4. 恢复输入提示符
```

**→ ERROR:**
```
1. 停止任何动画
2. 输出错误块:
   ┌─ Error ─────────────────────────┐
   │ Connection timeout              │
   │ Retry in 5s... (ctrl+r to retry)│
   └─────────────────────────────────┘
3. 状态栏更新为 "✗ error"
```

### 7.4 状态栏动画

状态栏的思考动画字符与输出区的思考动画同步, 均使用相同的 ● 闪烁动画, 由同一个计时器驱动。思考完成后 ● 变为静态绿色，失败变为静态红色。

---

## 八、Input Prompt 设计

### 8.1 提示符

**当前**: `> ` (简单大于号)

**新设计**: `❯ ` (Unicode 右指三角, U+276F)

```
❯ type your message here...
```

- 颜色: `ColorRole::Prompt` (黄色), 与当前一致
- `❯` 比 `>` 更有视觉辨识度
- 若终端不支持 Unicode, fallback 为 `> `

### 8.2 上下文提示

在多行输入时显示续行提示:

```
❯ First line of input \
│ continued here \
│ and more
```

- 续行使用 `│ ` 而非 `> `, 表示"仍在同一输入中"
- 颜色同 Prompt

### 8.3 自动补全内联显示

```
❯ /he│lp
     ──
```

- 补全候选以灰色虚线显示在光标后方
- 按 Tab 接受补全, 继续输入则忽略
- 仅在 `/` 开头（命令补全）或含路径分隔符时触发

### 8.4 输入行与状态栏的关系

输入行始终在状态栏上方（倒数第 2 行）。Status Bar 在最底部（最后一行）。状态栏更新时, 不影响输入行内容。

```
 ❯ |                                          ← 光标在这里（倒数第 2 行）
 ── Status Bar ──────────────────────────────  ← 最底部（最后一行）
```

---

## 九、完整交互流示例

### 9.1 完整对话流程

```
 ██████╗ ██████╗ ███████╗    Workx v0.2.0
 ██╔══██╗██╔══██╗██╔════╝    glm-5.1 · API Usage Billing
 ██║  ██║██████╔╝███████╗    D:\develop\Workspace\workx
 ██║  ██║██╔══██╗╚════██║
 ██████╔╝██║  ██║███████║    /help for commands
 ╚═════╝ ╚═╝  ╚═╝╚══════╝

 ❯ How do I implement a BFS algorithm?

 ● Thought for 3s (ctrl+o to expand)

 Here's a BFS implementation in C++:

 ┌─ cpp ───────────────────────────────────────┐
 │ #include <queue>                             │
 │ #include <vector>                            │
 │                                              │
 │ void bfs(const std::vector<std::vector<int>>&│
 │           graph, int start) {                │
 │     std::vector<bool> visited(graph.size()); │
 │     std::queue<int> q;                       │
 │     q.push(start);                           │
 │     while (!q.empty()) {                     │
 │         int node = q.front(); q.pop();       │
 │         if (visited[node]) continue;         │
 │         visited[node] = true;                │
 │         for (int next : graph[node])         │
 │             if (!visited[next])              │
 │                 q.push(next);                 │
 │     }                                        │
 │ }                                            │
 └──────────────────────────────────────────────┘

 856 tokens · 62.3 tok/s · 4.1s

 ❯ |
 ──[glm-5.1] │ workx │ ⏱ 1m07s │ idle─────────────
```

### 9.2 带工具调用的流程

```
 ❯ Refactor the parser to use modern C++

 ● Thought for 8s (ctrl+o to expand)

 I'll refactor the parser step by step.

 ⚙ ReadFile("src/parser.h")
   ⎿  156 lines · 4.1KB
 ⚙ ReadFile("src/parser.cpp")
   ⎿  423 lines · 12.8KB
 ⚙ EditFile("src/parser.h")
   ⎿  Applied 3 changes
 ⚙ EditFile("src/parser.cpp")
   ⎿  Applied 12 changes

 Here's a summary of the refactoring:
 • Replaced raw pointers with std::unique_ptr
 • Used std::string_view for non-owning references
 • Added constexpr where applicable

 2,341 tokens · 45.6 tok/s · 28.7s

 ❯ |
 ──[glm-5.1] │ workx │ ⏱ 2m15s │ idle─────────────
```

### 9.3 错误恢复流程

```
 ❯ Tell me a joke

 ┌─ Error ─────────────────────────────────┐
 │ Connection timeout                       │
 │ Retry in 5s... (ctrl+r to retry)         │
 └──────────────────────────────────────────┘

 ❯ |
 ──[glm-5.1] │ workx │ ⏱ 3m02s │ ✗ error──────────
```

用户按 ctrl+r:
```
 ● Thinking...

 Why did the C++ developer go broke?
 Because they used up all their cache.

 89 tokens · 78.1 tok/s · 1.1s
```

---

## 十、ColorScheme 扩展

### 10.1 新增 ColorRole

```cpp
enum class ColorRole {
    // 现有 (保持不变)
    Default,
    Prompt,
    UserInput,
    Assistant,
    Reasoning,
    System,
    Error,
    Command,
    ToolName,
    ToolOutput,
    Progress,

    // 新增
    StatusBar,          // 状态栏文字 (灰白)
    StatusBarBg,        // 状态栏背景 (深灰 237)
    ThinkingIndicator,  // 思考动画字符 ● (思考中: 青色闪烁, 完成: 绿色静态, 失败: 红色静态)
    ThinkingBlock,      // 思考块边框 (灰色)
    CodeBlock,          // 代码块边框 (深灰)
    CodeBlockBg,        // 代码块背景 (更深灰 236)
    Success,            // 成功标记 ✓ (绿色)
    Failure,            // 失败标记 ✗ (红色)
    TokenStats,         // Token 统计信息 (紫色, 同 System)
    ContextWarning,     // 上下文使用率警告 (红色, >90%)
    SplashLogo,         // Splash logo (青色, 同 Assistant)
    SplashInfo,         // Splash 信息 (紫色, 同 System)
};
```

### 10.2 ANSI 映射新增

```cpp
case ColorRole::StatusBar:       return "\x1b[37m";       // 白灰
case ColorRole::StatusBarBg:     return "\x1b[48;5;237m"; // 深灰背景
case ColorRole::ThinkingIndicator: return "\x1b[36m";     // 青（闪烁态）/ "\x1b[32m" 绿（完成）/ "\x1b[31m" 红（失败）
case ColorRole::ThinkingBlock:   return "\x1b[90m";       // 亮灰
case ColorRole::CodeBlock:       return "\x1b[90m";       // 亮灰
case ColorRole::CodeBlockBg:     return "\x1b[48;5;236m"; // 更深灰背景
case ColorRole::Success:         return "\x1b[32m";       // 绿
case ColorRole::Failure:         return "\x1b[31m";       // 红
case ColorRole::TokenStats:      return "\x1b[35m";       // 紫
case ColorRole::ContextWarning:  return "\x1b[31m";       // 红
case ColorRole::SplashLogo:      return "\x1b[36m";       // 青
case ColorRole::SplashInfo:      return "\x1b[35m";       // 紫
```

---

## 十一、实现策略

### 11.1 分层实施

**Phase 1: 状态机 + Status Bar (核心)**
- 新增 `TuiState` 枚举和 `TuiStateMachine` 类
- 实现状态栏渲染（固定位置, 反色背景）
- 重构 `Spinner` 为 `StatusBarAnimator`（管理状态栏动画）
- 修改 `ChatRenderer` 基于状态渲染

**Phase 2: Splash + Thinking**
- 实现新的 Splash Screen（ASCII art + 版本信息）
- 实现思考块折叠/展开（含 ctrl+o 交互）
- 计时器逻辑（思考持续时间）

**Phase 3: Streaming + Tool Calls**
- 实现 `StreamingBuffer` 优化
- 代码块 box-drawing 渲染
- 列表 bullet 替换
- 工具调用可视化（图标 + 嵌套缩进）
- 工具图标映射 + fallback

**Phase 4: 输入增强**
- 提示符更换为 `❯`
- 续行 `│ ` 显示
- 内联补全预览

### 11.2 文件变更清单

| 文件 | 变更类型 | 说明 |
|---|---|---|
| `src/tui/tui_state.h` | 新增 | TuiState 枚举 + TuiStateMachine 类 |
| `src/tui/status_bar.h/.cpp` | 新增 | 状态栏渲染 + 动画驱动 |
| `src/tui/splash.h/.cpp` | 新增 | Splash Screen 渲染 |
| `src/tui/thinking_block.h/.cpp` | 新增 | 思考块折叠/展开管理 |
| `src/tui/streaming_buffer.h/.cpp` | 新增 | Token 缓冲区 |
| `src/tui/tool_renderer.h/.cpp` | 新增 | 工具调用可视化 |
| `src/tui/code_block.h/.cpp` | 新增 | 代码块 box-drawing 渲染 |
| `src/tui/spinner.h/.cpp` | 重构 | → StatusBarAnimator 或废弃 |
| `src/tui/color_scheme.h/.cpp` | 修改 | 新增 ColorRole |
| `src/tui/chat_renderer.h/.cpp` | 修改 | 基于状态机渲染, 订阅重构 |
| `src/tui/terminal.h/.cpp` | 修改 | 提示符更新, splash 集成 |
| `src/tui/line_editor.h/.cpp` | 修改 | ctrl+o, 内联补全 |
| `src/session/events.h` | 修改 | 可能新增 ToolCallStartEvent 等 |

### 11.3 关键设计约束

1. **EventBus 不变**: 所有 TUI 变更仅影响订阅端, 不改变事件定义和发布端
2. **线程安全**: 所有渲染操作通过 `m_output_mutex` 保护, 新组件遵守同一模式
3. **Windows 兼容**: 所有 ANSI 序列在 Windows Terminal + ConPTY 上测试; 避免依赖 Windows Console API 特有功能
4. **Unicode fallback**: emoji 和特殊字符提供 ASCII fallback, 通过 `IPlatform` 能力检测选择
5. **窄终端适配**: 所有布局检测终端宽度, 提供简化 fallback

### 11.4 性能考虑

- StreamingBuffer 将 token 写入频率从 N次/秒 降低到 ~60次/秒
- Status Bar 每 200ms 刷新一次（思考动画帧率），不随 token 流刷新
- 代码块渲染在完整围栏结束后一次性 box-drawing，不在流式中实时变换
- 思考块展开/折叠使用 ANSI 光标操作原地替换，不重绘整个输出区
