# TUI Resize / Overlay 渲染管线（开发必读 · 踩坑记录）

<div align="center">

![鲸鱼娘训斥版（踩过的坑别再踩）](img/characters/03_render_pipeline_whale.jpg)

![TUI Resize/Overlay 渲染管线（严格按此顺序，否则光标乱飞/快照失效/死锁）](img/03_render_pipeline.jpg)

</div>

> ⚠️ **这不是参考，是硬性规定**。TUI 层任何改渲染/改 Resize 顺序/加 Overlay 面板的改动，必须严格按下图 5 步顺序执行。违反任何一步都会出现：**光标位置乱跳 / Screen 快照失效 / FTXUI PostEvent 递归死锁 / Unicode 宽字符截断半字符**。

---

## 5 步严格顺序（改 Resize 前必须背诵）

```
 ① 收集事件队列（poll input / window resize）
        │
        ▼
 ② Screen::Begin()  — 锁定双缓冲，取得 backbuffer 句柄
        │
        ▼
 ③ 按优先级分层渲染（最底层 transcript → widget → overlay panel → modal）
        │   每层必须调用 screen.At(x, y) 写入，禁止直接写 stdout / WriteConsole
        ▼
 ④ Screen::End()  — 计算 diff（backbuffer vs frontbuffer），生成最小增量字节序列
        │   （必须在 PostEvent 全部处理完之后调用，否则 diff 缺片段）
        ▼
 ⑤ Flush / Render 更新到真实终端（WriteConsoleW / write stdout）
```

### 每一步常见踩坑

| 步骤 | 违反后症状 | 真实事故记录 |
|------|-----------|-------------|
| ① 事件收集在 Begin() 之后 | Resize 后 backbuffer 尺寸还是旧的，屏幕右下 1/4 区域残留旧内容 | issue #17：窗口最大化后 ScrollRegion 越界崩溃 |
| ② 直接写 stdout / WriteConsole 跳过 Screen::At | 下次 diff 计算不到这块脏区，下一次刷新时被**擦掉**，出现"写了又闪消失" | issue #22：权限模态点 Yes 后文本短暂出现然后被清空 |
| ③ Overlay 渲染在 Screen::End() **之后** | diff 已经算完，Overlay 写进的字符完全不进 diff，下次刷新（如用户按键）直接被清 | issue #31：命令面板用鼠标滚动到最底下后消失 |
| ④ End() 里同步发 PostEvent（递归） | End() 调用回调 → 回调 publish 同步事件 → 另一个 subscriber 改 Screen → 又触发 End() → 死锁栈溢出 | issue #28：StatusBar 在 End() 里更新 Token 导致死循环 |
| ⑤ Flush 前改 Screen 状态 | Flush 的 diff 是 End() 算的，中间改状态 = Flush 不一致，光标 offset 错 1~2 列 | issue #35：CJK 宽字符输入时光标在字符左边半格 |

---

## 改渲染代码前 3 条 Checklist

1. **"这个写入发生在 Screen::Begin() 之后、Screen::End() 之前吗？"** — 任何不在这个区间里的直接写终端操作都是 bug 种子
2. **"Overlay/Modal 是通过 Screen::At(x, y) 写进 backbuffer，还是走了独立绘制函数？"** — 独立绘制函数会被 End() diff 机制漏掉
3. **"同步 publish(Event) 会不会在当前 End() / Begin() 调用链里重新进入？"** — 改 TUI 事件回调前先 grep `publish(`，确认是 `publish_async`（push 队列，下一轮 drain）还是 `publish`（同步调用所有 subscriber）

---

## 修复参考

对应源码目录：[`src/tui/core/`](../src/tui/core/)（screen.hpp / terminal_win32.cpp / terminal_posix.cpp）以及 [`src/tui/bridge/event_bridge.cpp`](../src/tui/bridge/event_bridge.cpp)（drain_async_events 的实现位置，Resize → Begin 的顺序就锁在这里）。

**如果遇到"渲染奇怪、调试看不出问题"**，99% 的情况是违反了 5 步顺序，回到上面 ASCII 流程图逐行比对调用顺序即可——别浪费时间排查 FTXUI bug。
