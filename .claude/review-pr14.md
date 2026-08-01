# Code Review 报告 — Issue #12 / PR #14

**审查范围：** PR #14 (`fix/issue-12-tui-resize` → `develop`)，commit `f7013d3`
**对比基线：** `e9f0d43`
**审查立场：** 假设改动有错误，反向举证
**审查结论：** ⚠️ CHANGES_REQUESTED

## 概览

| 项 | 统计 |
|----|------|
| 文件变更 | 12 files / +227 / -3 |
| 新增测试 | 0 cases |
| 测试总数 | 529 cases / 1910 assertions (全部通过) |
| mergeable | MERGEABLE (UNSTABLE) |
| 构建 | pass (MSVC Debug) |

## 验收标准核对

| 标准 | 状态 | 说明 |
|------|------|------|
| resize 时 `Thinking…` spinner 只显示一行 | ✅ | `m_last_rendered_row` 追踪 + `invalidate_last_row()` 强制重绘，[status_bar.cpp#L112-L123](file:///d:/develop/Workspace/workx/src/tui/widgets/status_bar.cpp#L112-L123) |
| resize 时 `● 思考…` 指示符无重复残留 | ⚠️ | 静态文本通过 DisplayBuffer 重放，但 H-1 数据竞争可能导致重放内容错乱 |
| resize 事件通过 EventBus 跨平台发布 | ✅ | POSIX SIGWINCH + Windows WINDOW_BUFFER_SIZE_EVENT，[platform_posix.cpp#L40-L42](file:///d:/develop/Workspace/workx/src/tui/core/platform/platform_posix.cpp#L40-L42), [platform_win32.cpp#L101-L104](file:///d:/develop/Workspace/workx/src/tui/core/platform/platform_win32.cpp#L101-L104) |
| 动画组件记录并擦除上次渲染行号 | ✅ | StatusBar 实现 `m_last_rendered_row`，通用机制可供其他组件复用 |
| dedup 缓存顺序修正 | ✅ | 先读 height 比较 `m_last_rendered_row`，再 dedup，[status_bar.cpp#L107-L123](file:///d:/develop/Workspace/workx/src/tui/widgets/status_bar.cpp#L107-L123) |
| resize 后 scroll region 与 DisplayBuffer 尺寸刷新 | ✅ | [terminal.cpp#L558-L575](file:///d:/develop/Workspace/workx/src/tui/core/terminal.cpp#L558-L575)（但时序问题见 H-2） |
| 现有单元测试全部通过 | ✅ | 529 cases / 1910 assertions 全通过 |
| 手动验证 | ⚠️ | 审查中无法执行，需开发者本地验证 |

## ⚠️ High 发现

### H-1. `handle_resize()` 中 `DisplayBuffer::snapshot()` 与后台 `feed()` 数据竞争

**位置：** [terminal.cpp#L551-L556](file:///d:/develop/Workspace/workx/src/tui/core/terminal.cpp#L551-L556)

**证据：**

```cpp
void Terminal::handle_resize() {
    // ...
    // 1. 快照旧滚动区域的可见内容（使用旧 DisplayBuffer 尺寸）
    std::vector<std::string> visible;
    if (m_display_buffer) {
        int old_scroll_h = old_h - 3;
        if (old_scroll_h < 1) old_scroll_h = 1;
        visible = m_display_buffer->snapshot(1, old_scroll_h);  // ← 未持 m_output_mutex
    }
```

后台事件泵线程在 [terminal.cpp#L119-L125](file:///d:/develop/Workspace/workx/src/tui/core/terminal.cpp#L119-L125) 每 50ms 调用 `event_bus().process_async_events()`，可能触发 `StreamTokenEvent` → `ChatRenderer` 回调 → `Terminal::write()` → 获取 `m_output_mutex` → `m_display_buffer->feed()`。

`snapshot()` 是 const 方法但读取 `m_total_rows` / `m_head` / `m_rows`，`feed()` 修改这些字段。两者并发构成数据竞争（C++ 标准 UB）。

**影响：**
- `m_head` 在 snapshot 读取过程中被修改，`idx = (physical-1) % m_capacity` 计算出错误索引，读到部分旧/部分新数据
- resize 后重放的对话历史可能有行错乱、内容丢失
- 长期运行下虽不崩溃（`m_rows` 固定大小无 reallocation），但违反 C++ 内存模型

**建议修复：**

```cpp
void Terminal::handle_resize() {
    int new_w = get_terminal_width();
    int new_h = get_terminal_height();
    int old_w = m_last_width;
    int old_h = m_last_height;

    if (new_w == old_w && new_h == old_h) return;

    // 1. 快照旧滚动区域（持锁，防止后台 feed() 并发修改）
    std::vector<std::string> visible;
    int old_scroll_h = old_h - 3;
    if (old_scroll_h < 1) old_scroll_h = 1;
    {
        std::lock_guard<std::mutex> lock(m_output_mutex);
        if (m_display_buffer) {
            visible = m_display_buffer->snapshot(1, old_scroll_h);
        }
        // 2. 清屏 + 重置 scroll region（在同一锁内，避免后台线程插入 write）
        m_platform->write_output("\x1b[r");
        m_platform->write_output("\x1b[2J\x1b[H");
        m_platform->flush();
        m_scroll_region_active = false;
        m_cursor_in_output = true;
    }

    // 3. 设置新 scroll region（setup_scroll_region 内部加锁）
    setup_scroll_region();

    // 4. 更新 DisplayBuffer 尺寸（持锁，防止 feed() 用旧尺寸折行）
    {
        std::lock_guard<std::mutex> lock(m_output_mutex);
        if (m_display_buffer) {
            m_display_buffer->set_width(new_w);
            m_display_buffer->set_height(new_h);
        }
    }
    // ... 后续重放逻辑同样持锁 ...
```

### H-2. `setup_scroll_region()` 与 `DisplayBuffer::set_width/height` 时序错位

**位置：** [terminal.cpp#L568-L575](file:///d:/develop/Workspace/workx/src/tui/core/terminal.cpp#L568-L575)

**证据：**

```cpp
    // 3. 设置新 scroll region（基于新高度）
    setup_scroll_region();          // ← 已用新高度设置 scroll region

    // 4. 更新 DisplayBuffer 尺寸
    if (m_display_buffer) {
        m_display_buffer->set_width(new_w);   // ← 此时 DisplayBuffer 仍是旧 width
        m_display_buffer->set_height(new_h);  // ← 仍是旧 height
    }
```

在步骤 3 和 4 之间，scroll region 已基于新高度，但 DisplayBuffer 仍用旧尺寸。若后台线程在此窗口期调用 `Terminal::write()` → `feed()`，feed 会用旧 width 折行，但实际终端已用新宽度渲染，导致折行不一致。

且 `set_width/set_height` 本身也无锁，与后台 `feed()` 并发访问 `m_width`/`m_height` 同样是数据竞争。

**影响：** resize 后对话历史的折行位置错误，长行可能在错误位置被截断。

**建议：** 见 H-1 修复方案，将 `set_width/set_height` 移入持锁块，且在 `setup_scroll_region()` 之前或同一锁内更新。

## ℹ️ Medium 发现

### M-1. `StatusBar::clear()` 未获取 `m_mutex`

**位置：** [status_bar.cpp#L138-L161](file:///d:/develop/Workspace/workx/src/tui/widgets/status_bar.cpp#L138-L161)

**证据：**

```cpp
void StatusBar::clear() {
    // ← 无 std::lock_guard<std::mutex> lock(m_mutex);
    int height = m_terminal->get_terminal_height();
    // ...
    if (m_last_rendered_row != 0 && m_last_rendered_row != status_row) {
        // 读写 m_last_rendered_row（与 render() 并发访问）
    }
    m_last_bar.clear();           // 读写 m_last_bar
    m_last_rendered_row = 0;
```

`render()` 持有 `m_mutex`，但 `clear()` 不持有。若 Spinner 线程调用 `render()` 同时主线程调用 `clear()`，`m_last_bar` / `m_last_rendered_row` 存在数据竞争。

**影响：** 理论上的数据竞争（UB）；实际 `clear()` 当前无调用方（grep 全代码库零匹配，见 M-2），影响可忽略。

**建议：** 给 `clear()` 加 `std::lock_guard<std::mutex> lock(m_mutex);`，与 `render()` 保持一致。

### M-2. `StatusBar::clear()` 为死代码

**位置：** [status_bar.cpp#L138](file:///d:/develop/Workspace/workx/src/tui/widgets/status_bar.cpp#L138)

**证据：** `grep -r "status_bar->clear\|status_bar\.clear\|->clear()" src/ tests/` 全代码库仅找到 `StatusBar::clear()` 定义处，无任何调用方。

**影响：** 死代码增加维护成本，且本次 PR 修改了它（新增 `m_last_rendered_row` 擦除逻辑），可能误导审查者以为它被使用。

**建议：** 若无计划使用，删除 `clear()` 方法及其声明；若计划使用（如未来 IDLE 转换时清理状态栏），保留并加注释说明预期调用场景。

### M-3. `handle_resize()` 未检查 `m_overlay_active`

**位置：** [terminal.cpp#L541-L616](file:///d:/develop/Workspace/workx/src/tui/core/terminal.cpp#L541-L616)

**证据：**

`begin_overlay` / `end_overlay` 在 [bottom_bar_manager.cpp#L78](file:///d:/develop/Workspace/workx/src/tui/widgets/bottom_bar_manager.cpp#L78)（CommandPanel）和 [model_selector.cpp#L66](file:///d:/develop/Workspace/workx/src/app/ui/model_selector.cpp#L66) 中使用。overlay 活动期间 `m_overlay_active = true`，此时 DisplayBuffer 暂停 feed。

`handle_resize()` 未检查 `m_overlay_active`，直接 `snapshot()` + 清屏 + 重放。若 resize 发生在 overlay 期间：
1. `snapshot()` 返回 overlay 之前的内容（非当前屏幕显示）
2. 清屏会擦除 overlay 显示
3. 重放的是 overlay 之前的内容，overlay 状态丢失

**影响：** 在 CommandPanel / ModelSelector 打开时调整终端大小，面板内容会被擦除并替换为历史快照。虽然概率较低（用户通常不在面板打开时 resize），但行为不符合预期。

**建议：** resize 时若 `m_overlay_active` 为 true，跳过重放逻辑（仅刷新 scroll region + 发布事件），或先 `end_overlay()` 再处理 resize。

### M-4. `handle_resize()` 分段加锁存在窗口期

**位置：** [terminal.cpp#L559-L604](file:///d:/develop/Workspace/workx/src/tui/core/terminal.cpp#L559-L604)

**证据：** `handle_resize()` 在步骤 2（清屏）、步骤 5（重放）分别获取 `m_output_mutex`，中间的 `setup_scroll_region()`（步骤 3）和 `set_width/height`（步骤 4）释放/重获锁。

```cpp
    {
        std::lock_guard<std::mutex> lock(m_output_mutex);  // 锁1
        // 清屏...
    }                                                      // 释放1
    setup_scroll_region();                                 // 内部锁2（独立）
    if (m_display_buffer) {
        m_display_buffer->set_width(new_w);                // 无锁！
        m_display_buffer->set_height(new_h);               // 无锁！
    }
    if (!visible.empty()) {
        std::lock_guard<std::mutex> lock(m_output_mutex);  // 锁3
        // 重放...
    }
```

后台线程可能在锁释放窗口期获取锁并调用 `write()`，导致输出在清屏后、重放前被写入，出现在错误的滚动区域位置。

**影响：** resize 期间可能有少量输出行位置错乱（视觉小瑕疵，非崩溃）。

**建议：** 将整个 resize 操作（清屏 + setup_scroll_region + set_size + 重放）放在单一锁内，或使用递归锁。注意 `setup_scroll_region()` 内部也获取 `m_output_mutex`，需重构为不持锁的内部版本。

### M-5. PR 未新增单元测试覆盖 resize 逻辑

**证据：** diff 显示 12 个文件 +227/-3 行，全部为生产代码，无测试文件变更。`tests/` 目录无新增文件。

**影响：** resize 逻辑涉及并发、事件发布、缓冲区重放等复杂行为，无测试保护后续重构易引入回归。验收标准"现有单元测试全部通过"达成，但无新增测试验证 resize 正确性。

**建议：** 至少为 `TerminalResizeEvent` 发布、`DisplayBuffer::snapshot`/`set_width`/`set_height` 组合行为添加单元测试；`handle_resize()` 的并发场景可用 mock 验证调用顺序。

## Low 发现

### L-1. POSIX `g_resize_pending` 为文件作用域全局变量

**位置：** [platform_posix.cpp#L38](file:///d:/develop/Workspace/workx/src/tui/core/platform/platform_posix.cpp#L38)

**证据：** `static volatile sig_atomic_t g_resize_pending = 0;` 是文件级全局变量。若创建多个 `PosixPlatform` 实例（测试场景可能），它们共享同一 flag。

**影响：** 实际生产环境单实例，无影响；测试场景若并行创建多个 platform 实例可能相互干扰。

**建议：** 可接受现状；若未来需要多实例测试，改为 `PosixPlatform` 静态成员或 thread_local。

### L-2. `m_last_width/m_last_height` 默认值与实际终端可能不一致

**位置：** [terminal.h#L228-L229](file:///d:/develop/Workspace/workx/src/tui/core/terminal.h#L228-L229)

**证据：** `int m_last_width = 80; int m_last_height = 24;`，但 `initialize()` 在 [terminal.cpp#L110-L111](file:///d:/develop/Workspace/workx/src/tui/core/terminal.cpp#L110-L111) 会立即用实际尺寸覆盖。

**影响：** 若 `initialize()` 与首次 resize 之间极短（理论场景），`old_w/old_h` 可能是 80/24 而非实际初始尺寸，导致 `TerminalResizeEvent` 的 old 值不准。实际影响极小。

**建议：** 可接受现状；若追求严谨，可在构造函数中查询实际尺寸初始化。

## 合并建议

**必须修复（阻断合并）：**
- H-1：`snapshot()` 调用加锁，消除与 `feed()` 的数据竞争
- H-2：`set_width/set_height` 加锁并在 `setup_scroll_region()` 之前更新

**建议修复（可合并后跟进）：**
- M-1：`StatusBar::clear()` 加锁
- M-3：`handle_resize()` 检查 `m_overlay_active`
- M-4：合并 resize 操作的加锁区间
- M-5：补充 resize 逻辑的单元测试

**可选修复：**
- M-2：删除或注释 `clear()` 死代码
- L-1 / L-2：保持现状

## 七维检测速查结果

| 维度 | 结果 | 说明 |
|------|------|------|
| 契约一致性 | ✅ | 新增 `ResizeCallback` / `invalidate_last_row()` / `handle_resize()` 签名一致，调用方已同步 |
| 并发与生命周期 | ⚠️ | H-1/H-2 数据竞争；M-1 clear() 无锁；M-4 分段加锁窗口 |
| 错误处理 | ✅ | 尺寸查询失败有默认值兜底；`m_resize_cb` 空检查 |
| 设计与可测试性 | ⚠️ | M-5 无新增测试；M-3 overlay 未处理 |
| 回归风险 | ✅ | 现有测试全通过；dedup 顺序修正不破坏现有行为 |
| 命名与文档 | ✅ | Doxygen 注释同步；命名遵循项目约定 |
| 提交规范 | ✅ | `fix(tui): #12` 遵循 Conventional Commits；`Closes #12` 关联 Issue |
