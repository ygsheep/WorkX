# 差分渲染 + Status Bar + 命令面板 + 选择面板 集成方案

## 一、当前渲染架构分析

### 1.1 两套渲染系统并存

| 系统 | 机制 | 当前用途 |
|------|------|---------|
| **Direct 模式** | `Terminal::write()` → `IPlatform::write_output()` 直接输出 | 主聊天循环（ChatRenderer, OutputFormatter, StreamingBuffer） |
| **Screen 差分模式** | 2D Cell 缓冲 → `flush()` 逐行对比 → 只写变化行 | SetupWizard、/model 选择器 |

**核心问题**：两套系统互不感知。StatusBar 用自己的"微差分"（字符串比较 `m_last_bar`），不走 Screen。

### 1.2 当前 StatusBar 渲染方式

```cpp
// status_bar.cpp:render()
// 1. 格式化状态栏字符串
// 2. 与 m_last_bar 比较（字符串级差分）
// 3. 直接用 ANSI 绝对定位写入最后一行
// 4. 恢复光标到输入行
```

- 位置：`height` 行（最底行），输入行在 `height - 1`
- 差分：**字符串比较**，内容不变则跳过
- 写入：单次 `write_safe()` 调用，避免多线程交叉

### 1.3 Screen 差分渲染能力

```cpp
// screen.cpp:flush()
// 1. 遍历 dirty 行
// 2. 逐 cell 对比当前帧 vs 上一帧
// 3. 变化的行 → render_line() 用 ANSI 绝对定位写入
// 4. 更新 previous 缓冲
```

- 支持 `write(row, col, text, color)` 精确写入
- 支持 `draw_box()` 绘制边框
- 支持 `fill()` 填充区域
- 行级 dirty 标记 + cell 级对比
- `clear_terminal()` 切换回 Direct 模式

---

## 二、集成方案：StatusBar 复用 Screen 差分渲染

### 2.1 可行性：✅ 完全可行

**理由**：

1. **Screen 已有精确的行定位渲染** — `render_line()` 用 `\x1b[row;1H` 定位，与 StatusBar 当前手动定位逻辑本质相同
2. **Screen 支持 cell 级差分** — 比 StatusBar 当前字符串比较更精确，闪烁 ● 只需更新 1 个 cell
3. **Screen 已被 SetupWizard 验证** — 交互式列表选择已有成熟实现（`draw_provider_list`）
4. **StatusBar 固定在最后一行** — 只需在 Screen 缓冲区的 `height - 1` 行写入，与 Screen 的 row 坐标完美对应

### 2.2 方案：StatusBar 区域纳入 Screen 管理

**核心思路**：将 StatusBar 占用的最后一行作为 Screen 缓冲区的一个固定区域，由 Screen 统一差分渲染。

```
Screen 缓冲区布局:
  row 0 ~ height-3:   [Output Area]    （暂不用，留给未来）
  row height-2:        [Input Line]     （暂不用，LineEditor 自管理）
  row height-1:        [Status Bar]     ← 由 StatusBar 写入 Screen
  row height-1:        [Command Panel]  ← 命令面板模式时替换
  row height-1:        [Select Panel]   ← 选择面板模式时替换
```

**但实际上，主聊天循环使用 Direct 模式，Screen 并不管理输出区**。所以更务实的方案是：

### 2.3 务实方案：StatusBar 内部使用 Screen 的差分引擎

**不改造主聊天循环的 Direct 模式**，而是让 StatusBar 内部维护一个 1 行的迷你 Screen 缓冲：

```cpp
class StatusBar {
    // 新增：1 行 Screen 缓冲，用于 cell 级差分
    std::vector<ScreenCell> m_current;   // 当前帧
    std::vector<ScreenCell> m_previous;  // 上一帧
    bool m_dirty = true;

    void render() {
        // 1. 格式化内容 → 写入 m_current
        // 2. cell-by-cell 对比 m_current vs m_previous
        // 3. 只写入变化的 cell（或整行重写，如果变化 cell 多）
        // 4. 更新 m_previous = m_current
    }
};
```

**优势**：
- 闪烁 ● 更新时只需写 1-2 个 cell，而不是整行重写
- 命令面板/选择面板切换时，只刷新变化的部分
- 不影响主聊天循环的 Direct 模式
- 与 Screen 的差分引擎逻辑一致，未来可统一

**简化实现**：由于 StatusBar 只有一行，且终端宽度通常 80-120 列，cell 级差分的收益有限。**推荐保持当前的字符串级差分**，但在命令面板/选择面板中采用 Screen 差分（因为它们有多行）。

### 2.4 最终推荐方案

| 组件 | 渲染方式 | 理由 |
|------|---------|------|
| StatusBar（1行） | 保持字符串差分 | 单行更新成本低，已有成熟实现 |
| CommandPanel（1行覆盖 StatusBar） | 直接 ANSI 写入（同 StatusBar） | 也是 1 行，复用 StatusBar 的渲染槽位 |
| SelectPanel（多行弹出） | 使用 Screen 差分渲染 | 多行选择器需要精确差分，避免闪烁 |

---

## 三、命令面板设计

### 3.1 触发机制

用户在输入行键入 `/` 时，StatusBar 区域切换为命令面板。

```
正常模式:
  ❯ |                                           ← Input Line
  ──[glm-5.1] │ workx │ ⏱ 1m23s │ idle──────── ← Status Bar

命令面板模式（输入 "/" 后）:
  ❯ /he│                                        ← Input Line
  /help       Show available commands            ← Command Panel (替换 Status Bar)
  /clear      Clear chat history
  /exit       Exit the application
```

### 3.2 交互

| 按键 | 行为 |
|------|------|
| 输入 `/` | 触发命令面板，显示匹配列表 |
| 继续输入 | 过滤匹配的命令（如 `/he` 匹配 `/help`） |
| `↑` / `↓` | 上下选择命令 |
| `Tab` | 补全当前选中的命令到输入行 |
| `Enter` | 补全 + 提交（或直接提交如果已完整） |
| `Escape` | 关闭命令面板，恢复 Status Bar |
| `Backspace` 删到 `/` 之前 | 关闭命令面板 |

### 3.3 命令面板数据模型

```cpp
struct CommandEntry {
    std::string name;        // "help"
    std::string description; // "Show available commands"
    std::string usage;       // "/help [command]"
};

class CommandPanel {
public:
    void set_commands(const std::vector<CommandEntry>& commands);
    void set_filter(const std::string& prefix);  // "/he" → 过滤
    void move_selection(int delta);               // ↑↓
    const CommandEntry* get_selected() const;
    std::string get_completion() const;           // Tab 补全文本

    /// 渲染命令面板到 Status Bar 区域（覆盖 Status Bar）
    void render(Terminal* terminal, int status_row, int input_row);

    /// 关闭面板，恢复 Status Bar
    void dismiss(StatusBar& status_bar);

    bool is_active() const { return m_active; }

private:
    std::vector<CommandEntry> m_all_commands;
    std::vector<size_t> m_filtered_indices;  // 过滤后的索引
    int m_selected = 0;                       // 当前选中
    bool m_active = false;
};
```

### 3.4 命令面板渲染

命令面板 **占据 Status Bar 所在的行**（屏幕最后一行），但可以扩展为多行（最多 N 行，向上覆盖输出区）。

**单行模式**（默认，StatusBar 区域）:
```
 /help       Show available commands  ▸
```
只显示当前选中的命令，`▸` 表示有更多选项。

**多行模式**（按 ↓ 展开或自动展开，最多 5 行）:
```
 /help       Show available commands  ◂   ← 当前选中（高亮）
 /clear      Clear chat history
 /exit       Exit the application
 /regen      Regenerate last response
 /save       Save session to file
```

**渲染机制**：
- 覆盖 Status Bar 行 + 上方 N-1 行
- 使用 ANSI 绝对定位，与 StatusBar 相同的手法
- 选中行使用 `\x1b[7m` 反色高亮
- 关闭时重写被覆盖的行（需要保存被覆盖内容，或清空后让输出区自然滚动）

**简化方案**：命令面板只用 1 行（Status Bar 区域），显示当前选中命令。↑↓ 切换时原地更新。这样不需要保存/恢复输出区内容。

### 3.5 与 LineEditor 的协作

当前 `LineEditor::read_line()` 内部循环处理按键。命令面板需要拦截 ↑↓ 和 Tab：

**方案 A：在 LineEditor 内处理**（推荐）

在 LineEditor 的按键循环中，检测到 `/` 开头时进入命令面板模式：
- `↑` / `↓` → 转发给 CommandPanel 而不是 history
- `Tab` → 调用 CommandPanel 补全，更新 LineEditor 内容
- 其他输入 → 正常编辑 + 更新 CommandPanel 过滤器

**方案 B：在 Terminal::run_advanced() 中处理**

在 read_line() 返回后处理，但这意味着 ↑↓ 无法实时控制命令面板。

**选 A**，在 LineEditor 内部增加命令面板回调。

```cpp
// LineEditor 新增
using CommandPanelCallback = std::function<void(const std::string& line, char32_t key)>;
void set_command_panel_callback(CommandPanelCallback cb);
```

当 LineEditor 检测到 `m_line` 以 `/` 开头时：
- ↑↓ 转发给 callback 而不是 history
- Tab 转发给 callback
- 每次内容变化调用 callback 更新过滤

---

## 四、选择面板设计

### 4.1 概念

选择面板是一个多选 UI，用于让用户在多个分类（Tab）中选择选项。

```
 [Models] [Providers]          ← Tab 栏，Tab 键切换
 ◉ glm-5.1                    ← 空格选中/取消，◉=选中 ○=未选
 ○ claude-sonnet-5
 ○ gpt-4o
 ● deepseek-r1                 ← ●=当前光标所在行
 ○ llama-3.1-70b
```

### 4.2 触发场景

- `/model` 命令 → 选择模型
- `/provider` 命令 → 选择 Provider
- 任何需要多选/单选的场景

### 4.3 数据模型

```cpp
struct SelectTab {
    std::string name;                    // "Models"
    std::vector<SelectItem> items;
};

struct SelectItem {
    std::string id;                      // "glm-5.1"
    std::string display;                 // "glm-5.1 (recommended)"
    bool selected = false;               // 空格切换
};

class SelectPanel {
public:
    /// 设置 Tab 列表
    void set_tabs(const std::vector<SelectTab>& tabs);

    /// Tab 切换
    void next_tab();
    void prev_tab();

    /// 上下移动光标
    void move_up();
    void move_down();

    /// 空格选中/取消当前项
    void toggle_current();

    /// 获取所有选中项
    std::vector<std::string> get_selected_ids() const;

    /// 渲染（使用 Screen 差分渲染）
    void render(Screen& screen);

    /// 获取面板高度
    int panel_height() const;

private:
    std::vector<SelectTab> m_tabs;
    int m_active_tab = 0;
    int m_cursor_row = 0;       // 当前光标行（在当前 Tab 的 items 中）
};
```

### 4.4 布局

选择面板覆盖输出区的一部分（从底部向上展开）：

```
┌──────────────────────────────────────────────┐
│                                              │
│ [Output Area]  (被部分覆盖)                    │
│                                              │
├──────────────────────────────────────────────┤
│ [Models] [Providers]                         │ ← Tab 栏
│ ◉ glm-5.1                                    │ ← 选择列表
│ ○ claude-sonnet-5                            │
│ ○ gpt-4o                                     │
│ ● deepseek-r1                                │
│ ○ llama-3.1-70b                              │
│ ↑↓ navigate  Tab switch  Space select  Enter confirm │ ← 提示行
├──────────────────────────────────────────────┤
│ ❯ /model                                     │ ← Input Line
│ ──[glm-5.1] │ workx │ ⏱ 1m23s │ idle────── │ ← Status Bar
└──────────────────────────────────────────────┘
```

### 4.5 渲染：使用 Screen 差分

选择面板是最适合使用 Screen 差分渲染的组件：

1. **多行交互式 UI** — 光标移动、选中切换都需要精确刷新
2. **频繁局部更新** — 空格切换只改 1 个字符（○ → ◉），光标移动只改 2 行（旧行去掉 ●，新行加 ●）
3. **已有验证** — SetupWizard 的 `draw_provider_list()` 就是这个模式

**实现策略**：

```cpp
void SelectPanel::render(Screen& screen) {
    int start_row = screen.height() - panel_height() - 2; // -2: input + statusbar

    // Tab 栏
    int col = 0;
    for (int i = 0; i < m_tabs.size(); i++) {
        auto color = (i == m_active_tab) ? ColorRole::Prompt : ColorRole::Dim;
        screen.write(start_row, col, "[" + m_tabs[i].name + "]", color);
        col += m_tabs[i].name.size() + 3;
    }

    // 选项列表
    const auto& items = m_tabs[m_active_tab].items;
    for (int i = 0; i < items.size(); i++) {
        int row = start_row + 1 + i;
        char sel = items[i].selected ? '\xe2\x97\x89' : '\xe2\x97\x8b'; // ◉ / ○
        char cur = (i == m_cursor_row) ? '\xe2\x97\x8f' : ' ';          // ● / 空格

        screen.write(row, 0, std::string(1, cur) + " " + std::string(1, sel) + " " + items[i].display,
                     (i == m_cursor_row) ? ColorRole::UserInput : ColorRole::Default);
    }

    // 提示行
    int hint_row = start_row + 1 + items.size();
    screen.write(hint_row, 0, "↑↓ navigate  Tab switch  Space select  Enter confirm", ColorRole::Dim);

    screen.flush();
}
```

### 4.6 模式切换流程

```
Terminal::run_advanced() 主循环:
  IDLE 状态, 用户输入 /model
  → CommandEvent 发布
  → main.cpp 的 CommandEvent 订阅者捕获
  → 创建 SelectPanel, 进入选择循环
  → 选择循环中使用 Screen 差分渲染
  → 用户按 Enter 确认
  → Screen::clear_terminal() 清空
  → 返回 IDLE，恢复 Direct 模式
```

这与当前 `/model` 命令的流程完全一致（已使用 Screen 差分渲染），只是 UI 从简单列表升级为 Tab 分组 + 多选。

---

## 五、StatusBar / CommandPanel / SelectPanel 统一管理

### 5.1 BottomBar 抽象

三种面板共享屏幕底部区域，需要统一管理切换：

```cpp
enum class BottomBarMode {
    STATUS_BAR,      // 默认状态栏
    COMMAND_PANEL,   // 命令面板（输入 / 触发）
    SELECT_PANEL,    // 选择面板（/model 等命令触发）
};

class BottomBarManager {
public:
    BottomBarManager(Terminal* terminal);

    /// 切换模式
    void set_mode(BottomBarMode mode);

    /// 获取当前模式
    BottomBarMode mode() const { return m_mode; }

    /// 渲染当前活跃的面板
    void render();

    // 委托方法
    StatusBar& status_bar();
    CommandPanel& command_panel();
    SelectPanel& select_panel();

private:
    Terminal* m_terminal;
    BottomBarMode m_mode = BottomBarMode::STATUS_BAR;
    StatusBar m_status_bar;
    CommandPanel m_command_panel;
    SelectPanel m_select_panel;
};
```

### 5.2 状态转换

```
STATUS_BAR ──(用户输入 /)──→ COMMAND_PANEL
COMMAND_PANEL ──(Escape / 删掉 /)──→ STATUS_BAR
COMMAND_PANEL ──(Enter 提交命令)──→ STATUS_BAR
COMMAND_PANEL ──(选择 /model)──→ SELECT_PANEL
SELECT_PANEL ──(Enter 确认)──→ STATUS_BAR
SELECT_PANEL ──(Escape 取消)──→ STATUS_BAR
```

### 5.3 键盘路由

```
LineEditor 检测输入内容:
  - 空行 或 非命令 → 正常编辑（↑↓ = history，Tab = completion）
  - / 开头 → COMMAND_PANEL 模式
    - ↑↓ → CommandPanel::move_selection()
    - Tab → CommandPanel::补全到 LineEditor
    - 其他 → 正常编辑 + 更新 filter

SELECT_PANEL 活跃时:
  - 所有键盘输入由 SelectPanel 处理（类似 SetupWizard）
  - LineEditor 暂停
```

---

## 六、新增 ColorRole

```cpp
// 命令面板
CommandPanelBg,        // 命令面板背景 (同 StatusBarBg)
CommandPanelHighlight, // 命令面板选中行 (反色或高亮)
CommandPanelDesc,      // 命令描述 (灰色)

// 选择面板
SelectTab,             // Tab 标签 (正常: 灰色, 活跃: 黄色/Prompt)
SelectTabActive,       // 活跃 Tab 标签 (高亮)
SelectCursor,          // 选择面板光标 ● (青色)
SelectChecked,         // 已选中 ◉ (绿色)
SelectUnchecked,       // 未选中 ○ (灰色)
```

---

## 七、文件变更清单

| 文件 | 变更类型 | 说明 |
|------|---------|------|
| `src/tui/command_panel.h/.cpp` | 新增 | 命令面板组件 |
| `src/tui/select_panel.h/.cpp` | 新增 | 选择面板组件（Tab 分组 + 多选） |
| `src/tui/bottom_bar_manager.h/.cpp` | 新增 | 底部区域统一管理器 |
| `src/tui/status_bar.h/.cpp` | 修改 | 接入 BottomBarManager |
| `src/tui/line_editor.h/.cpp` | 修改 | 新增命令面板回调，↑↓ 路由 |
| `src/tui/terminal.h/.cpp` | 修改 | 集成 BottomBarManager |
| `src/tui/color_scheme.h` | 修改 | 新增 ColorRole |
| `src/session/command_router.h/.cpp` | 修改 | 命令定义增加描述字段 |
| `src/main.cpp` | 修改 | /model 命令使用 SelectPanel |

---

## 八、实施顺序

**Phase 1: BottomBarManager + CommandPanel**
1. 新增 `BottomBarManager` 类
2. 新增 `CommandPanel` 组件
3. 修改 `LineEditor` 增加 ↑↓ 路由
4. 修改 `StatusBar` 接入 Manager
5. 验证：输入 `/` → 命令面板显示，↑↓ 选择，Tab 补全

**Phase 2: SelectPanel**
1. 新增 `SelectPanel` 组件（Tab 分组 + 多选）
2. 修改 `/model` 命令使用 SelectPanel
3. 使用 Screen 差分渲染
4. 验证：`/model` → 选择面板，Tab 切换分组，Space 选中

**Phase 3: 优化与集成**
1. 命令面板多行展开模式
2. StatusBar 内部 cell 级差分（可选优化）
3. 窄终端 fallback
4. 键盘快捷键文档
