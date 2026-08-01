# Code Review 重审报告 — Issue #12 / PR #14

**审查范围：** PR #14 commit `bd034eb`（针对首轮 review 的修复）
**对比基线：** `f7013d3`（首轮审查的 commit）
**审查立场：** 假设改动有错误，反向举证
**审查结论：** ✅ APPROVED

## 概览

| 项 | 统计 |
|----|------|
| 文件变更 | 3 files / +78 / -50 |
| 测试总数 | 529 cases / 1910 assertions (全部通过) |
| mergeable | MERGEABLE (CLEAN) |
| 构建 | pass (MSVC Debug, 无 error/warning) |

## 首轮发现修复核对

### H-1: `snapshot()` 与 `feed()` 数据竞争 — ✅ 已修复

**修复前：** `snapshot()` 在锁外调用，与后台 `feed()` 并发访问 `m_total_rows`/`m_head`/`m_rows`。

**修复后：** [terminal.cpp#L567-L578](file:///d:/develop/Workspace/workx/src/tui/core/terminal.cpp#L567-L578) 整个 `snapshot()` 调用纳入 `m_output_mutex` 保护内。后台 `feed()` 也需获取同一锁，数据竞争已消除。

**验证：** 读取 [terminal.cpp#L389-L412](file:///d:/develop/Workspace/workx/src/tui/core/terminal.cpp#L389-L412) 确认 `Terminal::write()` 中 `feed()` 调用仍在 `m_output_mutex` 保护内，锁粒度一致。

### H-2: `set_width/height` 时序错位 — ✅ 已修复

**修复前：** `set_width/set_height` 在 `setup_scroll_region()` 之后、无锁调用。

**修复后：** [terminal.cpp#L588-L596](file:///d:/develop/Workspace/workx/src/tui/core/terminal.cpp#L588-L596)
- `set_width/set_height` 移到 `setup_scroll_region_locked()` 之前
- 在 `m_output_mutex` 保护内执行
- 确保后台 `feed()` 在锁保护下看到新尺寸

**验证：** 顺序为 `snapshot → 清屏 → set_width/height → setup_scroll_region_locked → 重放`，逻辑正确。

### M-1: `StatusBar::clear()` 未获取 `m_mutex` — ✅ 已修复

**修复后：** [status_bar.cpp#L139](file:///d:/develop/Workspace/workx/src/tui/widgets/status_bar.cpp#L139) 添加 `std::lock_guard<std::mutex> lock(m_mutex);`，与 `render()` 保持一致。

### M-3: `handle_resize()` 未检查 `m_overlay_active` — ✅ 已修复

**修复后：** [terminal.cpp#L572-L625](file:///d:/develop/Workspace/workx/src/tui/core/terminal.cpp#L572-L625) 用 `if (!m_overlay_active)` 包裹整个快照/清屏/重放逻辑。overlay 期间仅更新 `m_last_width/m_last_height`，不擦除面板显示。

### M-4: 分段加锁窗口期 — ✅ 已修复

**修复前：** 锁1(清屏) → 锁2(setup_scroll_region) → 锁3(重放)，存在窗口期。

**修复后：**
- [terminal.cpp#L567-L629](file:///d:/develop/Workspace/workx/src/tui/core/terminal.cpp#L567-L629) 整个 resize 操作纳入单一 `m_output_mutex` 锁内
- 新增 `setup_scroll_region_locked()` [terminal.cpp#L483-L496](file:///d:/develop/Workspace/workx/src/tui/core/terminal.cpp#L483-L496) 避免在持锁状态下重复加锁导致死锁
- `event_bus().publish()` 在锁外执行 [terminal.cpp#L632-L638](file:///d:/develop/Workspace/workx/src/tui/core/terminal.cpp#L632-L638)，避免回调中再次获取锁导致死锁

**验证：** grep 确认 `setup_scroll_region_locked()` 仅被 `handle_resize()` 调用，无其他调用方误用风险。

## 新增问题检查

### N-1: `setup_scroll_region_locked()` 为 public 方法 — ℹ️ Low

**位置：** [terminal.h#L81](file:///d:/develop/Workspace/workx/src/tui/core/terminal.h#L81)

**分析：** 该方法设计意图是"调用方必须已持有 `m_output_mutex`"，但声明为 public。其他代码可能在不持锁的情况下误用。

**实际影响：** grep 确认当前仅 `handle_resize()` 调用，无实际风险。

**建议：** 可改为 private 方法，通过注释说明调用约束。可不阻断合并。

### N-2: overlay 期间 DisplayBuffer 尺寸未更新 — ℹ️ Low

**位置：** [terminal.cpp#L572-L625](file:///d:/develop/Workspace/workx/src/tui/core/terminal.cpp#L572-L625)

**分析：** overlay 期间跳过了 `set_width/set_height`（在 `if (!m_overlay_active)` 块内）。若 overlay 期间 resize，`m_last_width/m_last_height` 更新了，但 DisplayBuffer 仍是旧尺寸。

**实际影响：**
- overlay 期间 `feed()` 被跳过（`!m_overlay_active` 为 false），不会有错误折行
- overlay 结束后流式内容恢复 `feed()`，此时 DisplayBuffer 仍是旧尺寸
- 但 `end_overlay()` 后通常会触发完整重绘，且下次 resize 会同步尺寸

**结论：** 这是边缘场景（overlay 期间 resize），且不会导致崩溃或明显视觉问题。可作为后续优化。

### N-3: `m_last_width/m_last_height` 锁外读取 — ℹ️ Low

**位置：** [terminal.cpp#L559-L560](file:///d:/develop/Workspace/workx/src/tui/core/terminal.cpp#L559-L560)

**分析：** `int old_w = m_last_width; int old_h = m_last_height;` 在锁外读取。写入在锁内（L627-L628）。

**实际影响：** `handle_resize()` 由平台事件循环单线程调用，不存在并发 resize。读取虽在锁外，但无实际数据竞争。

**结论：** 可接受现状。若追求严谨，可将读取移入锁内。

## 动态验证结果

| 项 | 结果 |
|----|------|
| MSVC Debug 构建 | ✅ pass（无 error/warning） |
| 单元测试 | ✅ 529 cases / 1910 assertions 全通过 |

## 合并建议

**✅ 可以合并**

**理由：**
1. 首轮审查的 2 个 High（H-1 数据竞争、H-2 时序错位）均已正确修复
2. 首轮审查的 3 个 Medium（M-1 clear 无锁、M-3 overlay 未检查、M-4 分段加锁）均已正确修复
3. 修复未引入新的 High/Medium 问题
4. 新增的 3 个 Low 问题均为设计层面的优化建议，不阻断合并
5. 构建通过，全部测试通过
6. `mergeStateStatus: CLEAN`，无合并冲突

**剩余建议（可合并后跟进）：**
- M-2（首轮）：`StatusBar::clear()` 死代码，可删除
- M-5（首轮）：补充 resize 逻辑的单元测试
- N-1：`setup_scroll_region_locked()` 改为 private
- N-2：overlay 期间同步 DisplayBuffer 尺寸（边缘场景优化）
- L-1/L-2（首轮）：保持现状

## 七维检测速查结果

| 维度 | 结果 | 说明 |
|------|------|------|
| 契约一致性 | ✅ | 新增 `setup_scroll_region_locked()` 签名一致，Doxygen 注释说明调用约束 |
| 并发与生命周期 | ✅ | H-1/H-2 数据竞争已消除；M-1 clear() 加锁；M-4 单一锁内完成；publish 在锁外避免死锁 |
| 错误处理 | ✅ | 尺寸计算有兜底；overlay 检查防止状态破坏 |
| 设计与可测试性 | ⚠️ | N-1 public 方法可改 private；M-5 仍无新增测试（可后续跟进） |
| 回归风险 | ✅ | 现有测试全通过；修复未改变对外行为 |
| 命名与文档 | ✅ | 注释清晰说明修复意图；命名遵循项目约定 |
| 提交规范 | ✅ | `fix(tui): #12 code review 修复` 遵循 Conventional Commits；commit body 列出修复项 |
