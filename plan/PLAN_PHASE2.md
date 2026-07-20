# WorkX 修复 Plan — 阶段 2：渲染核心 P0（硬约束 + 中文支持）

> **状态**：待审批（未做任何代码修改）
> **范围**：仅修复渲染相关的 P0 问题，共 7 项
> **依据**：[CODE_REVIEW_REPORT.md](file:///c:\Users\young\Desktop\Develop\WorkX\CODE_REVIEW_REPORT.md) 第 6 节、第 7 节
> **用户硬约束**（来自 project_memory）：
>   - 代码渲染格式：`│N  content`（U+2502 左侧连接符、行号右对齐、行号与内容间两空格、无右边框）
>   - 代码块渲染禁止使用四角框线字符 (┌─┐└┘)，但允许使用 │ 作为左侧连接竖线
>   - Diff highlighting 必须用 256-color 背景色（addition `\x1b[48;5;22m`，deletion `\x1b[48;5;52m`），保留前景语法高亮
>   - Diff prefixes (`+`, `-`, `+++`, `---`) 与 hunk headers (`@@ ... @@`) 不得显示

---

## 0. 阶段 2 修复范围

| 编号 | 问题 | 文件 | 类别 |
|------|------|------|------|
| 6.1 | Markdown 代码块使用禁止的四角框线字符 | `tui/render/markdown_renderer.cpp` | 违反硬约束 |
| 6.2 | Screen::draw_box 垂直循环用宽度参数 | `tui/core/screen.{h,cpp}` | 渲染致命 bug |
| 6.3 | Cell 空判定用 `"\0"` 字符串比较 | `tui/core/screen.cpp` | 渲染致命 bug |
| 6.4 | LineEditor::estimate_width 始终返回 1 | `tui/input/line_editor.cpp` | 中文支持致命 |
| 6.5 | LineEditor Backspace 删除宽字符只清 1 个空格 | `tui/input/line_editor.cpp` | 6.4 的衍生 |
| 6.16 | UTF-8 宽度判定范围不全 | `tui/utils/utf8_utils.cpp` | 中文/emoji 支持 |
| 7.1 | Win32 平台编码不一致 | `tui/core/platform/platform_win32.cpp` | 编码致命 bug |
| **合计** | **7 项** | **6 个文件** | |

> 6.17（Tab 宽度 0）与 6.18（4 字节字符一律 width=2 未处理 ZWJ）合并到 6.16 一并修复
> 6.13（markdown_renderer 变量命名 inner_h 实为宽度）合并到 6.1 修复（代码块重写时一并解决）

---

## 1. Markdown 代码块改造（违反硬约束 + 命名错误，2 项合并）

### 1.1 问题清单
- **6.1** `render_code_block` 使用 `BOX_TL/BOX_TR/BOX_BL/BOX_BR/BOX_H`（`┌┐└┘─`）绘制四角框线
- **6.13** `int inner_h = max_w + 2;` 变量名 `inner_h` 实为宽度，后续 `h_after = inner_h - 1 - display_width(lang) - 2` 用错值算高度

### 1.2 当前代码位置
- [markdown_renderer.cpp:148-153](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\render\markdown_renderer.cpp#L148) — BOX_* 常量定义
- [markdown_renderer.cpp:574-641](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\render\markdown_renderer.cpp#L574) — `render_code_block` 函数
- [markdown_renderer.cpp:287, 301](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\render\markdown_renderer.cpp#L287) — 表格 `make_border` 也用四角（不在本次范围，仅标注）

### 1.3 修复方案：改为 `│N  content` 格式

**目标渲染效果**（无顶/底/右边框，仅左侧 `│` 竖线 + 行号）：

```
│1   #include <iostream>
│2   
│3   int main() {
│4       std::cout << "你好" << std::endl;
│5       return 0;
│6   }
```

**关键规则**（来自 project_memory）：
1. 左侧连接符：`│`（U+2502，UTF-8 编码 `\xe2\x94\x82`），上下行无缝衔接
2. 行号 `N` 右对齐（按代码块总行数的位数定宽，最少 1 位）
3. 行号与内容之间**两个空格**
4. **无右边框、无顶/底框线**
5. 行号使用 `ansi::GRAY` 着色，内容保留语法高亮原色

**新实现**（替换 [markdown_renderer.cpp:574-641](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\render\markdown_renderer.cpp#L574)）：

```cpp
std::string render_code_block(std::string_view lang,
                               const std::vector<std::string>& lines) {
    std::ostringstream os;

    // 1. 整块交给语法高亮器
    std::string joined;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) joined.push_back('\n');
        joined += lines[i];
    }
    std::string highlighted = highlight_code(lang, joined);

    // 2. 重新按 \n 拆行
    std::vector<std::string> hl_lines;
    {
        std::string cur;
        for (char c : highlighted) {
            if (c == '\n') { hl_lines.push_back(std::move(cur)); cur.clear(); }
            else cur.push_back(c);
        }
        hl_lines.push_back(std::move(cur));
        if (hl_lines.size() != lines.size()) {
            hl_lines = lines;  // 防御性回退
        }
    }

    // 3. 计算行号宽度（按总行数位数）
    const int total_lines = static_cast<int>(hl_lines.size());
    int line_num_width = 1;
    for (int n = total_lines; n >= 10; n /= 10) ++line_num_width;

    // 4. 渲染：左侧 │ + 行号右对齐 + 两空格 + 内容（无右边框）
    //    顶部可选加一行语言标签（无 ┌─┐ 框线，仅文本）
    if (!lang.empty()) {
        os << ansi::GRAY << lang << ansi::RESET << "\n";
    }

    const std::string box_v = "\xe2\x94\x82";  // │ U+2502
    for (int i = 0; i < total_lines; ++i) {
        const std::string& l = hl_lines[i];
        // 行号右对齐
        std::string num_str = std::to_string(i + 1);
        std::string num_padding;
        if (static_cast<int>(num_str.size()) < line_num_width) {
            num_padding.append(line_num_width - num_str.size(), ' ');
        }

        // │ + 行号(灰) + 两空格 + 内容(保留高亮)
        os << ansi::GRAY << box_v << ansi::RESET
           << ansi::GRAY << num_padding << num_str << ansi::RESET
           << "  "
           << l
           << "\n";
    }

    return os.str();
}
```

### 1.4 清理的旧代码
- `BOX_TL/BOX_TR/BOX_BL/BOX_BR/BOX_H` 常量在 `render_code_block` 中不再使用
- **保留** `BOX_V` 常量（新代码使用）和表格相关常量（表格暂不改）
- 删除 `inner_h` 错误命名变量（6.13 一并解决）

### 1.5 风险与缓解
- **视觉变化大**：原四角框线 → 仅左竖线，用户可能不适应。**缓解**：硬约束明确要求，无回旋余地
- **行号宽度对齐**：行号位数随代码块大小变化，相邻代码块可能不对齐。**缓解**：单代码块内部对齐即可，跨块不强求
- **语法高亮兼容性**：`highlight_code` 返回带 ANSI 的字符串，行号拼接在 ANSI 之前不影响。**验证**：手动测试 C++/Python/diff 三种语言
- **空行处理**：空行内容为空字符串，输出 `│1  ` 后直接换行，符合预期

---

## 2. Screen::draw_box 垂直循环 bug 修复（1 项 P0）

### 2.1 问题清单
- **6.2** [screen.cpp:107](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\core\screen.cpp#L107) `for (int r = row + 1; r < row + 1 + inner_w - 2; r++)` 用 `inner_w`（宽度）限制垂直行循环

### 2.2 当前签名
```cpp
void Screen::draw_box(int row, int col, int width, const std::string& title);
```
**问题**：缺少 `height` 参数，无法正确绘制高瘦盒子。

### 2.3 修复方案

#### 2.3.1 增加 height 参数（[screen.h](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\core\screen.h)）

```cpp
/// @brief 绘制带标题的方框
/// @param row 顶行位置（0-based）
/// @param col 左列位置（0-based）
/// @param width 总宽度（含边框，>=4）
/// @param height 总高度（含边框，>=3）
/// @param title 标题文本
void draw_box(int row, int col, int width, int height, const std::string& title);
```

#### 2.3.2 修复循环条件（[screen.cpp:92-119](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\core\screen.cpp#L92)）

```cpp
void Screen::draw_box(int row, int col, int width, int height, const std::string& title) {
    if (width < 4 || height < 3 || row + height > m_height || col + width > m_width) return;

    int inner_w = width - 2;   // 内部宽度（不含左右边框）
    int inner_h = height - 2;  // 内部高度（不含上下边框）

    // 顶行: ╔═ title ═╗
    write(row, col, "\xe2\x95\x94", ColorRole::StatusBar);  // ╔
    write(row, col + 1, " " + title + " ", ColorRole::StatusBar);
    int fill_start = col + 1 + static_cast<int>(title.size()) + 1;
    for (int i = fill_start; i < col + inner_w; i++) {
        write(row, i, "\xe2\x95\x90", ColorRole::StatusBar);  // ═
    }
    write(row, col + inner_w, "\xe2\x95\x97", ColorRole::StatusBar);  // ╗

    // 中间行：用 inner_h 控制行数（修复 6.2）
    for (int r = row + 1; r < row + 1 + inner_h; r++) {
        write(r, col, "\xe2\x95\x91", ColorRole::StatusBar);           // ║
        write(r, col + inner_w, "\xe2\x95\x91", ColorRole::StatusBar); // ║
    }

    // 底行
    int bottom = row + height - 1;
    write(bottom, col, "\xe2\x95\x9a", ColorRole::StatusBar);  // ╚
    for (int i = col + 1; i < col + inner_w; i++) {
        write(bottom, i, "\xe2\x95\x90", ColorRole::StatusBar);  // ═
    }
    write(bottom, col + inner_w, "\xe2\x95\x9d", ColorRole::StatusBar);  // ╝
}
```

#### 2.3.3 调用方更新

需搜索所有 `draw_box(` 调用点，补充 `height` 参数。预期调用点：
- SetupWizard、SelectPanel、CommandPanel、FileSearchPanel 等 widgets

**Plan 标记**：执行阶段需用 Grep 找出所有调用点逐一更新。

### 2.4 风险与缓解
- **接口变更影响调用方**：所有 `draw_box` 调用必须更新。**缓解**：编译期检查（缺参数报错）；逐一对照原代码意图推算合理 height 值
- **测试**：测试「宽矮盒子（width=80, height=5）」与「高瘦盒子（width=20, height=15）」渲染

---

## 3. Cell 空判定修复（1 项 P0）

### 3.1 问题清单
- **6.3** [screen.cpp:229](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\core\screen.cpp#L229) `if (cell.width == 0 && cell.ch == "\0")` 判定错误
- `"\0"` 是长度 1 的字符串字面量（包含一个空字节），`cell.ch`（`std::string`）几乎永远不等于它
- 实际 `cell.ch` 取值：`" "`（空格，初始化值）或实际字符；宽字符延续位未见任何代码设置为 `"\0"`

### 3.2 修复方案

#### 3.2.1 明确"宽字符延续标记"语义

查阅代码，宽字符（width=2）应占用 2 个 cell：第 1 个 cell `width=2, ch="<char>"`，第 2 个 cell `width=0, ch=""`（延续标记）。

**当前问题**：从未有代码设置 `cell.ch = ""`（清空），所有 cell 默认 `ch=" "`。延续标记无法识别。

#### 3.2.2 修改空判定（[screen.cpp:229](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\core\screen.cpp#L229)）

```cpp
// 修改前：
// if (cell.width == 0 && cell.ch == "\0") {

// 修改后：用 width==0 作为唯一判定（ch 不参与）
if (cell.width == 0) {
    // 宽字符延续位：跳过（由前一个 cell 的字符覆盖显示）
    if (cell.color != current_color) {
        flush_segment();
        current_color = cell.color;
    }
    continue;
}
```

#### 3.2.3 修正宽字符写入逻辑

需确认 `Screen::write` 系列函数在写宽字符时是否正确设置后续 cell 的 `width=0`。**Plan 标记**：执行阶段需读取 `Screen::write` 实现，若未设置延续位需补上：

```cpp
// 假设 write_char_at(row, col, ch, width):
if (width == 2) {
    cell.ch = ch;
    cell.width = 2;
    // 显式设置延续位
    if (col + 1 < m_width) {
        m_lines[row].cells[col + 1].ch = "";   // 清空，标记为延续
        m_lines[row].cells[col + 1].width = 0;
        m_lines[row].cells[col + 1].color = cell.color;
    }
}
```

### 3.3 风险与缓解
- **行为变化**：原来 `width==0` 的 cell 总是被作为字符输出（因 ch 不等于 "\0"），改后跳过。**缓解**：实际现网代码从未设置 `width==0`，所以行为应无变化；但若 set_codepoint 路径设置了 width==0 但 ch=" "，原代码会输出空格，新代码会跳过 → 视觉上少一个空格但布局正确
- **测试**：测试「CJK 字符串输入到 Screen」「宽字符位于行末自动换行」

---

## 4. LineEditor::estimate_width 修复 + Backspace 衍生问题（2 项 P0）

### 4.1 问题清单
- **6.4** [line_editor.cpp:136-141](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\input\line_editor.cpp#L136) `estimate_width` 始终返回 1
- **6.5** [line_editor.cpp:495-523](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\input\line_editor.cpp#L495) Backspace 删除宽字符只清 1 个空格（根源是 6.4）

### 4.2 当前调用链分析

```
用户输入字符 → LineEditor::handle_input (line 528)
  → w = estimate_width(cp)        // 始终 1
  → real_w = put_codepoint(..., w)  // Win32 实测返回真实宽度
  → m_widths.push_back(real_w)    // 实际宽度被记录
```

**关键发现**：`m_widths` 数组实际存的是 `put_codepoint` 的返回值（真实宽度），不是 `estimate_width` 的返回值。

**那为什么还需要修 estimate_width？**
- [line_editor.cpp:197-200](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\input\line_editor.cpp#L197) `expected_width` 传给 `put_codepoint`
- [platform_win32.cpp:182](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\core\platform\platform_win32.cpp#L182) `if (!GetConsoleScreenBufferInfo(...)) return expected_width;` — 失败时回退
- [platform_win32.cpp:199-200](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\core\platform\platform_win32.cpp#L199) 真实 width 计算失败时也可能回退

**实际影响**：
1. Win32 上 put_codepoint 大部分情况能拿到真实宽度，estimate_width 错误被掩盖
2. POSIX 平台 put_codepoint 实现未读，可能直接返回 expected_width → 中文宽度全错
3. 行末边界判定（line_editor.cpp:179-180 `total_width = accumulate(m_widths)`）依赖 m_widths，理论上正确
4. **真正问题**：若 put_codepath 任何分支回退到 expected_width，中文宽度立即错乱

### 4.3 修复方案

#### 4.3.1 实现 estimate_width 真实判定（[line_editor.cpp:136-141](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\input\line_editor.cpp#L136)）

```cpp
int LineEditor::estimate_width(char32_t codepoint) {
    // 控制字符宽度为 0
    if (codepoint < 0x20) return 0;

    // ASCII 可打印字符宽度为 1
    if (codepoint < 0x7F) return 1;

    // 调用统一的 utf8_utils 宽度判定（避免逻辑重复）
    // 注：utf8_utils 当前接收 char32_t，可直接复用
    return char32_width(codepoint);
}
```

#### 4.3.2 在 utf8_utils 中抽出 char32_width 公共函数

当前 `decode_utf8_cells` 内联了宽度判定逻辑（[utf8_utils.cpp:18-48](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\utils\utf8_utils.cpp#L18)）。抽取为独立函数供 estimate_width 与 decode_utf8_cells 共用：

```cpp
// utf8_utils.h 新增
int char32_width(char32_t cp);

// utf8_utils.cpp 新增
int char32_width(char32_t cp) {
    if (cp < 0x20) return 0;
    if (cp < 0x7F) return 1;

    // 2 字节字符
    if (cp >= 0x80 && cp <= 0x7FF) {
        // 拉丁扩展等通常宽度 1
        return 1;
    }

    // 3 字节字符（CJK 范围）
    if (cp >= 0x800 && cp <= 0xFFFF) {
        if (cp >= 0x1100 && (
            (cp <= 0x115F) ||
            (cp >= 0x2E80 && cp <= 0xA4CF && cp != 0x303F) ||
            (cp >= 0xAC00 && cp <= 0xD7A3) ||
            (cp >= 0xF900 && cp <= 0xFAFF) ||
            (cp >= 0xFE30 && cp <= 0xFE6F) ||
            (cp >= 0xFF01 && cp <= 0xFF60) ||
            (cp >= 0xFFE0 && cp <= 0xFFE6))) {
            return 2;
        }
        return 1;
    }

    // 4 字节字符（emoji 等）
    if (cp >= 0x10000) {
        // 隐式 6.18 留待后续：ZWJ 序列需整体判定，此处单 codepoint 判定
        // Emoji 与符号范围宽度 2
        if (cp >= 0x1F300 && cp <= 0x1FAFF) return 2;  // Emoji & symbols
        if (cp >= 0x1F000 && cp <= 0x1F02F) return 2;  // Mahjong
        if (cp >= 0x1F0A0 && cp <= 0x1F0FF) return 2;  // Playing cards
        if (cp >= 0x2600 && cp <= 0x26FF) return 2;    // Misc symbols
        if (cp >= 0x2700 && cp <= 0x27BF) return 2;    // Dingbats
        if (cp >= 0x2B00 && cp <= 0x2BFF) return 2;    // Misc symbols & arrows
        // CJK 扩展 E/F/G/H/I
        if (cp >= 0x2B700 && cp <= 0x2B73F) return 2;  // Ext E
        if (cp >= 0x2B740 && cp <= 0x2B81F) return 2;  // Ext F
        if (cp >= 0x2B820 && cp <= 0x2CEAF) return 2;  // Ext G
        if (cp >= 0x2CEB0 && cp <= 0x2EBEF) return 2;  // Ext H
        if (cp >= 0x30000 && cp <= 0x3134F) return 2;  // Ext I
        return 1;
    }

    return 1;
}
```

`decode_utf8_cells` 改为调用 `char32_width`，避免逻辑重复。

#### 4.3.3 Backspace 修复（[line_editor.cpp:495-523](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\input\line_editor.cpp#L495)）

**核心结论**：Backspace 代码本身是正确的，它使用 `m_widths[m_char_pos - 1]` 作为 `w` 来清空残余空格。只要 `m_widths` 数组中存的是真实宽度（已通过 put_codepoint 保证），Backspace 就能正确清空。

**潜在问题**：若 put_codepoint 在某些情况回退到 expected_width=1，则 m_widths 存的是 1，Backspace 也只清 1 个空格 → 遗留空格。

**修复 6.4 后**：estimate_width 返回真实宽度，即使 put_codepath 回退也能正确清空。

**无需修改 Backspace 代码本身**，6.5 通过修复 6.4 自动解决。

### 4.4 风险与缓解
- **POSIX 平台 put_codepoint 行为未知**：若直接返回 expected_width，修复 6.4 后即可正确。**缓解**：执行阶段读取 platform_posix.cpp 的 put_codepoint 实现确认
- **m_widths 数组与显示不同步**：若 put_codepoint 返回的 real_width 与 expected_width 不一致，可能引起已渲染字符的宽度对齐问题。**缓解**：现有代码已处理（line 535-543 用 real_w 入数组，put_codepoint 已实际渲染）

---

## 5. UTF-8 宽度判定范围扩展（1 项 P0，含 6.17/6.18 部分）

### 5.1 问题清单
- **6.16** [utf8_utils.cpp:33-44](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\utils\utf8_utils.cpp#L33) 缺少 emoji、杂项符号、CJK 扩展 E/F/G/H/I 范围
- **6.17** [utf8_utils.cpp:22](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\utils\utf8_utils.cpp#L22) Tab 宽度为 0
- **6.18** 4 字节字符一律 width=2，未处理 ZWJ 序列（部分留待后续）

### 5.2 修复方案

#### 5.2.1 Tab 宽度修复（[utf8_utils.cpp:22](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\utils\utf8_utils.cpp#L22)）

```cpp
// 修改前：
// if (c < 0x20 && c != '\t') width = 0;  // 控制字符宽度为 0

// 修改后：Tab 视为 4 列宽（与多数编辑器默认一致；硬约束未指定 Tab 宽度）
if (c < 0x20) {
    width = (c == '\t') ? 4 : 0;
}
```

**注**：Tab 宽度 4 是保守选择（vim 默认 8，VSCode 默认 4）。可后续抽到配置。硬约束未指定，先取 4。

#### 5.2.2 4 字节字符范围细化（[utf8_utils.cpp:45-48](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\utils\utf8_utils.cpp#L45)）

```cpp
// 修改前：
// } else if ((c & 0xF8) == 0xF0) {
//     len = 4;
//     width = 2;  // Emoji 通常宽度为 2
// }

// 修改后：调用 char32_width
} else if ((c & 0xF8) == 0xF0) {
    len = 4;
    if (i + 3 < text.size()) {
        char32_t cp = (static_cast<char32_t>(c & 0x07) << 18)
            | (static_cast<char32_t>(static_cast<unsigned char>(text[i+1]) & 0x3F) << 12)
            | (static_cast<char32_t>(static_cast<unsigned char>(text[i+2]) & 0x3F) << 6)
            | static_cast<char32_t>(static_cast<unsigned char>(text[i+3]) & 0x3F);
        width = char32_width(cp);
    } else {
        width = 2;  // 截断的 4 字节序列回退
    }
}
```

#### 5.2.3 3 字节字符也用 char32_width

将 [utf8_utils.cpp:29-44](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\utils\utf8_utils.cpp#L29) 内联的 CJK 宽度判定替换为 `char32_width(cp)` 调用，统一逻辑入口。

#### 5.2.4 ZWJ 序列（6.18 部分留待后续）

ZWJ（U+200D）连接的 emoji 序列如 `👨‍👩‍👧‍👦` 应整体宽度 2，但当前按单 codepoint 判定每个组成字符各计 2 → 总宽度 8+。

**本次不修复**，仅记录：
- 需要 lookahead 解析 ZWJ 序列
- 实现复杂度高，且 LineEditor 上下文中 ZWJ 序列罕见
- 标记为 P1 留待阶段 3 或 4

### 5.3 风险与缓解
- **Tab 宽度 4 与现有渲染不兼容**：若有代码假设 Tab 宽度 0 或 8，可能错位。**缓解**：搜索 Tab 在渲染路径中的使用，确认无依赖
- **新增 emoji 范围误判**：某些 emoji 实际宽度 1（如部分 variation selector）。**缓解**：保守取 2，符合多数终端行为

---

## 6. Win32 平台编码一致性修复（1 项 P0）

### 6.1 问题清单
- **7.1** [platform_win32.cpp:58](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\core\platform\platform_win32.cpp#L58) `_setmode(_fileno(stdin), _O_WTEXT)` 设 stdin 为 UTF-16
- 但 [platform_win32.cpp:127-138](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\core\platform\platform_win32.cpp#L127) `write_output` 用 `WriteConsoleA` 写 UTF-8 字节
- stdin（UTF-16）与 stdout（UTF-8）编码不匹配

### 6.2 分析

**关键发现**：
1. `read_char` 实现使用 `ReadConsoleInputW`（[platform_win32.cpp:82-125](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\core\platform\platform_win32.cpp#L82)），**不依赖 stdin 模式**
2. `write_output` 用 `WriteConsoleA` 写 UTF-8 字节，**不依赖 stdout 模式**
3. `_setmode(_fileno(stdin), _O_WTEXT)` 仅影响 `_getws`/`fgetws`/`fgetwc` 等 CRT 函数，对 `ReadConsoleInputW` 无影响

**结论**：`_O_WTEXT` 设置是历史遗留（可能从某模板抄来），实际无作用，但与 `disable_raw_mode` 中的 `_O_U8TEXT` 不一致，且让代码读者困惑。

**真正的潜在问题**：若代码中任何位置调用 `fgetws`/`getwchar` 等 CRT 输入函数，会因 `_O_WTEXT` 期望 UTF-16 但实际读取 ANSI/UTF-8 而出错。当前 `read_char` 用 `ReadConsoleInputW` 规避了此问题。

### 6.3 修复方案

#### 6.3.1 统一 stdin 模式为 UTF-8（[platform_win32.cpp:58](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\core\platform\platform_win32.cpp#L58)）

```cpp
// 修改前：
// _setmode(_fileno(stdin), _O_WTEXT);

// 修改后：与 disable_raw_mode 一致使用 _O_U8TEXT
_setmode(_fileno(stdin), _O_U8TEXT);
```

**理由**：
- `_O_U8TEXT` 与 `disable_raw_mode` 中的设置一致（[platform_win32.cpp:76](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\core\platform\platform_win32.cpp#L76)）
- 与 `WriteConsoleA` + `SetConsoleOutputCP(CP_UTF8)` 形成的 UTF-8 输出链路对称
- 即使后续代码用 CRT 函数读 stdin，也是 UTF-8 模式

#### 6.3.2 SetConsoleOutputCP 检查返回值（隐含修复 7.6）

```cpp
// 修改前：
// SetConsoleOutputCP(CP_UTF8);

// 修改后：
if (!SetConsoleOutputCP(CP_UTF8)) {
    // 失败时记录但不阻塞（旧版 Windows 仍可工作，仅 UTF-8 显示乱码）
    // 输出到 stderr 避免污染主输出流
    std::cerr << "Warning: SetConsoleOutputCP(CP_UTF8) failed, UTF-8 display may be incorrect\n";
}
```

### 6.4 风险与缓解
- **若代码中确有依赖 `_O_WTEXT` 的隐藏路径**：改为 `_O_U8TEXT` 后该路径会出错。**缓解**：搜索 `_fileno(stdin)`/`fgetws`/`getwc` 等使用点，确认无依赖
- **测试**：测试「输入中文字符 → 显示正确」「输入 emoji → 显示正确」「Ctrl+C 中断」

---

## 7. 阶段 2 实施顺序

建议按依赖关系分批实施：

### 批次 1：UTF-8 基础设施（无依赖）
1. **utf8_utils 抽取 char32_width** + 扩展宽度范围 + Tab 宽度（第 5 节）
   - 其他修复都依赖此函数

### 批次 2：平台与底层（依赖批次 1）
2. **Win32 编码一致性**（第 6 节）— 不依赖批次 1，可并行
3. **LineEditor estimate_width**（第 4 节）— 依赖 char32_width
4. **Screen::draw_box 修复**（第 2 节）— 独立
5. **Screen Cell 空判定**（第 3 节）— 独立

### 批次 3：渲染层（依赖批次 1-2）
6. **Markdown 代码块改造**（第 1 节）— 独立但视觉变化大，最后改便于对比

### 批次 4：验证
7. 编译通过（MSVC /W4）
8. 运行 `build/bin/Debug/example_code_highlight.exe` 验证代码块渲染格式
9. 手动测试：输入中文、删除中文、CJK 字符串显示对齐
10. 手动测试：SetupWizard/SelectPanel 等使用 draw_box 的界面
11. 手动测试：Win32 平台输入 emoji、特殊符号

---

## 8. 验证清单

阶段 2 完成后需验证：

### 8.1 编译与基础
- [ ] 编译无 warning（MSVC /W4）
- [ ] `example_code_highlight.exe` 运行正常

### 8.2 代码块格式（硬约束符合性）
- [ ] 代码块无 `┌┐└┘─` 四角框线
- [ ] 代码块左侧为 `│`（U+2502）竖线，上下行无缝衔接
- [ ] 行号右对齐
- [ ] 行号与内容间恰好两个空格
- [ ] 无右边框
- [ ] 代码块内语法高亮保留
- [ ] 多个代码块连续渲染对齐正确

### 8.3 中文/emoji 支持
- [ ] 输入中文字符 → 光标位置正确（每个中文占 2 列）
- [ ] Backspace 删除中文字符 → 清空 2 个空格，无残留
- [ ] Left/Right 在中文字符间移动 → 每次移动 2 列
- [ ] 输入 emoji（如 😀）→ 显示宽度 2
- [ ] 输入杂项符号（如 ☀ ★）→ 显示宽度 2
- [ ] CJK 扩展字符（如 𠮷）→ 显示宽度 2

### 8.4 Screen 渲染
- [ ] `draw_box(0, 0, 80, 5, "test")` 渲染宽矮盒子正确
- [ ] `draw_box(0, 0, 20, 15, "test")` 渲染高瘦盒子正确
- [ ] 宽字符位于行末 → 自动换行到下一行
- [ ] 全屏刷新无残留空格

### 8.5 Win32 平台
- [ ] 输入「你好」→ 显示「你好」（非乱码）
- [ ] 输入 emoji → 正常显示
- [ ] Ctrl+C → 正常中断
- [ ] 程序退出后终端恢复正常模式

---

## 9. 待审批事项

请审阅本 Plan 并确认：

1. **代码块新格式是否符合预期？**
   - 顶部是否需要语言标签行（当前方案：单独一行 `lang` 文本，无框线）？
   - 行号是否需要始终显示？还是仅对超过 N 行的代码块显示？
   - 行号颜色（当前 GRAY）是否合适？

2. **`draw_box` 接口变更**（增加 height 参数）是否可接受？需更新所有调用方。

3. **Tab 宽度取 4** 是否合适？还是改 8？

4. **ZWJ emoji 序列**（6.18）确认留待后续阶段？

5. **utf8_utils 抽出 char32_width 函数** 是否可接受？（新增公共 API）

6. **是否进入执行阶段？**

待你审批后，我将按批次顺序执行修改。整个阶段 2 预计涉及 6 个文件的修改：
- `tui/utils/utf8_utils.{h,cpp}` — 新增 char32_width + 扩展范围
- `tui/core/screen.{h,cpp}` — draw_box 加 height + Cell 判定修复
- `tui/input/line_editor.cpp` — estimate_width 实现
- `tui/render/markdown_renderer.cpp` — 代码块重写
- `tui/core/platform/platform_win32.cpp` — 编码一致性

无新增文件。
