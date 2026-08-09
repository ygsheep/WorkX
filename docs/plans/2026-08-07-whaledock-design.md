# WhaleDock 灵动岛 GUI 设计文档

> 日期：2026-08-07
> 状态：已确认设计，待实施
> 关联：WorkX TUI (workx.exe) + 独立 GUI (whale-dock) + DearTs 框架

---

## 目录

1. [设计目标与范围](#1-设计目标与范围)
2. [整体架构与进程拓扑](#2-整体架构与进程拓扑)
3. [跨平台支持矩阵](#3-跨平台支持矩阵)
4. [IPC 协议与事件 Schema](#4-ipc-协议与事件-schema)
5. [GUI 状态机与岛形态渲染](#5-gui-状态机与岛形态渲染)
6. [费用统计逻辑与余额拉取](#6-费用统计逻辑与余额拉取)
7. [插件划分、目录结构与启动流程](#7-插件划分目录结构与启动流程)
8. [错误处理与测试策略](#8-错误处理与测试策略)
9. [里程碑与风险](#9-里程碑与风险)

---

## 1. 设计目标与范围

### 1.1 核心目标

构建一个**独立的跨平台 GUI 应用**（基于 DearTs 框架），以 macOS 灵动岛形态展示：

- **DeepSeek 账户余额**（剩余可用额度）
- **本次任务花费**（单个 user turn 的 token 折算成本）
- **模型单价表**（DeepSeek 各模型输入/输出/缓存单价）
- **任务完成程度**（实时活动状态：思考中 / 工具调用 / 完成）

### 1.2 关键约束

- GUI 是**独立应用**，不是 TUI 的附庸
- **跨平台**：Windows + macOS + Linux 全支持
- 使用 **DearTs 框架**（SDL3 + ImGui）开发
- 与 TUI 通过 **IPC 联动**，支持自动连接与独立常驻两种模式
- 渲染层用 **SDL3_Renderer 单份代码**，零平台特化
- 仅 IPC 传输与系统托盘需平台特化

### 1.3 数据来源

| 数据 | 来源 | 拉取方式 |
|------|------|----------|
| 账户余额 | DeepSeek `/user/balance` API | 低频拉取（启动 + 10min + 任务完成） |
| 本次任务花费 | TUI 的 LLM usage 数据 | TUI 端 CostAccumulator 累积，IPC 推送 |
| 模型单价表 | 硬编码 fallback + 用户配置 | GUI 首次连接时 `get_model_pricing` 拉取 |
| 任务进度 | TUI 的 EventBus 事件流 | IPC 实时推送 |

---

## 2. 整体架构与进程拓扑

```
┌─────────────────── TUI 进程 (workx.exe) ──────────────────┐
│                                                            │
│  ReAct Loop ──► agent::EventBus                            │
│                     │                                      │
│                     ▼                                      │
│             IslandIpcServer (新增 src/island/)            │
│             ──────────────────────────────                 │
│             • 启动时创建 IPC 端点                          │
│               Win: \\.\pipe\workx-island-<pid>             │
│               Unix: /tmp/workx-island-<uid>-<pid>.sock     │
│             • 订阅 EventBus，序列化为 JSONL 推送          │
│             • 接收 GUI 请求（如「刷新余额」）              │
│             • 发布注册文件：~/.workx/island.registry       │
│               {pid, pipe_path, started_at, project_root}   │
└─────────────────────────┬──────────────────────────────────┘
                          │ JSONL 事件流（单向推送）
                          │ + 请求/响应（双向）
                          ▼
┌──────────────── DearTs GUI 进程 (WhaleDock.exe) ────────┐
│                                                            │
│  DearTsApplication                                         │
│  ┌──────────────────────────────────────────────────────┐   │
│  │  Plugin: whale_dock_core  (主插件)                │   │
│  │  • IslandView (ViewFloating, 置顶·无装饰·透明)     │   │
│  │  • IslandController (状态机：胶囊/扩展/隐藏)        │   │
│  │  • 订阅本地 EventBus，渲染灵动岛                    │   │
│  └──────────────────────────────────────────────────────┘   │
│  ┌──────────────────────────────────────────────────────┐   │
│  │  Plugin: whale_dock_ipc  (IPC 客户端)             │   │
│  │  • 后台任务：连接 TUI 的 IPC 端点                   │   │
│  │  • 读 JSONL → 发布到 DearTs EventBus                │   │
│  │  • 暴露命令："连接 TUI"/"断开"/"刷新余额"           │   │
│  │  • 自动重连 + 心跳                                   │   │
│  └──────────────────────────────────────────────────────┘   │
│  ┌──────────────────────────────────────────────────────┐   │
│  │  Plugin: deepseek_billing  (费用统计)               │   │
│  │  • 后台任务：每 10min 拉 GET /user/balance           │   │
│  │  • 累计本次任务花费（按 ToolResultEvent 的 usage）   │   │
│  │  • 维护模型单价表（deepseek-v4-flash 等）           │   │
│  │  • 提供 ViewWindow「DeepSeek 账单」查看历史         │   │
│  └──────────────────────────────────────────────────────┘   │
└────────────────────────────────────────────────────────────┘
```

### 2.1 关键设计决策

- GUI 是独立的 DearTs 应用，用户可手动启动并常驻托盘
- TUI 启动时把 IPC 端点写入 `~/.workx/island.registry`（含 pid 用于存活检测）
- GUI 启动时读取注册文件、连接 TUI；TUI 退出后 GUI 自动重连等待
- 一个 GUI 可连接多个 TUI（用 Tab 切换），完全独立运行
- 费用计算放 TUI 端（拥有真实 usage 数据），GUI 只做展示与历史聚合

---

## 3. 跨平台支持矩阵

### 3.1 平台支持表

| 平台 | 窗口/渲染 | IPC 传输 | 系统托盘 | 注册文件路径 |
|------|-----------|----------|----------|----------------|
| **Windows** | SDL3 + SDL_Renderer | named pipe `\\.\pipe\workx-island-<pid>` | `Shell_NotifyIconW` | `%USERPROFILE%\.workx\island.registry` |
| **macOS** | SDL3 + SDL_Renderer | Unix socket `$TMPDIR/workx-island-<uid>-<pid>.sock` | `NSStatusItem` (ObjC++) | `~/.workx/island.registry` |
| **Linux** | SDL3 + SDL_Renderer | Unix socket `$XDG_RUNTIME_DIR/workx-island-<uid>-<pid>.sock` | StatusNotifierItem D-Bus | `~/.workx/island.registry` |

### 3.2 渲染层简化

SDL3 的 `SDL_Renderer` 已抽象掉 D3D11/Metal/Vulkan 差异，**全平台一份代码**：

```cpp
// 跨平台一份代码：SDL_CreateRenderer 自动选择后端
SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);

// ImGui 后端也是一份
ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
ImGui_ImplSDLRenderer3_Init(renderer);

// 透明合成：SDL_SetRenderDrawColor alpha < 255 即透明
SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
SDL_RenderClear(renderer);  // 配合 SDL_PROP_WINDOW_CREATE_TRANSPARENT_BOOLEAN
```

### 3.3 IPC 传输层抽象

新增 `core/ipc/` 模块（位于 DearTs 框架层）：

```cpp
// core/ipc/itransport.h
namespace DearTs::Core::Ipc {

class ITransport {
public:
    virtual ~ITransport() = default;
    virtual bool listen(const std::string& endpoint) = 0;       // TUI 端
    virtual bool connect(const std::string& endpoint) = 0;      // GUI 端
    virtual ssize_t read_nonblocking(std::span<std::byte> buf) = 0;
    virtual ssize_t write(std::span<const std::byte> data) = 0;
    virtual void close() = 0;
    [[nodiscard]] virtual bool is_connected() const = 0;
};

// 端点路径生成（跨平台）
std::string default_endpoint(uint32_t pid);  // 平台特化
std::vector<RegistryEntry> discover_peers();  // 扫描 ~/.workx/island.registry

}
```

- 用 standalone **asio**（header-only）做异步 I/O
- Windows 实现：`CreateNamedPipe` + `ConnectNamedPipe`
- POSIX 实现：`socket(AF_UNIX)` + `bind/listen/accept`
- 端点路径：Linux 用 `$XDG_RUNTIME_DIR`，macOS 用 `$TMPDIR`

### 3.4 透明置顶窗口（SDL3 原生）

```cpp
SDL_PropertiesID props = SDL_CreateProperties();
SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_TRANSPARENT_BOOLEAN, true);
SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_BORDERLESS_BOOLEAN, true);
SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_ALWAYS_ON_TOP_BOOLEAN, true);
SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, 380);
SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, 56);
```

### 3.5 跨平台降级矩阵

| 功能 | Windows | macOS | Linux (X11) | Linux (Wayland) |
|------|---------|-------|-------------|-----------------|
| 透明窗口 | 完整 | 完整 | 完整 | 需 compositor |
| 置顶 | 完整 | 完整 | 完整 | 协议限制 |
| 鼠标穿透 | 完整 Win32 | 完整 NSWindow | 完整 X11 Shape | 不支持 |
| 托盘菜单 | 完整 | 完整 | 完整 SNI | 看 compositor |

### 3.6 平台额外配置

**macOS**：
- `Info.plist` 需声明 `LSUIElement = true`（无 Dock 图标，仅菜单栏应用）
- 应用签名 + 公证（hardened runtime）

**Linux**：
- Wayland 下透明窗口、always-on-top 支持受限，降级为普通置顶
- 托盘作为可选项：检测到支持才显示，不支持时降级为快捷键 `Ctrl+Alt+I` 显示/隐藏
- 建议 AppImage 分发

---

## 4. IPC 协议与事件 Schema

### 4.1 协议总览

IPC 上跑的是**双通道 JSONL**：
- **事件流**（TUI → GUI，单向推送）：每行一个 JSON 事件
- **请求/响应**（双向，带 `id` 关联）：GUI 主动查询或控制 TUI

所有消息共享外层信封：

```json
{"kind": "event", "type": "tool_call", "seq": 42, "ts": 1789456789.123, "data": {...}}
{"kind": "request", "type": "refresh_balance", "id": "req-1", "data": {}}
{"kind": "response", "id": "req-1", "ok": true, "data": {...}}
```

- `seq`：TUI 单调递增序号，GUI 用于断线重连后从上次位置续传
- `ts`：Unix 时间戳（秒·毫秒），GUI 用于排序与去重
- `id`：请求 ID，响应用同 `id` 关联

### 4.2 事件类型 Schema（TUI → GUI）

| 事件 type | 触发源（agent EventBus） | data 字段 | 用途 |
|-----------|--------------------------|-----------|------|
| `session_started` | SessionStartEvent | `{session_id, project_root, model, provider, pid, started_at}` | GUI 显示会话徽标、初始化 |
| `tool_call` | ToolCallEvent | `{call_id, tool_name, tool_type, arguments, started_at}` | 岛扩展显示「正在调用 Bash」 |
| `tool_result` | ToolResultEvent | `{call_id, tool_name, is_error, duration_ms, result_preview, usage?}` | 岛显示「✓ 完成 / ✗ 失败」 |
| `thinking_started` | ThinkingStartedEvent | `{iteration}` | 岛显示思考动画 |
| `thinking_delta` | ThinkingDeltaEvent | `{delta_text}` | 累积思考内容（ctrl+o 展开用，可选订阅） |
| `thinking_done` | ThinkingDoneEvent | `{duration_ms, token_count}` | 岛收起到胶囊 |
| `message_delta` | MessageDeltaEvent | `{delta_text}` | 流式回复预览（岛扩展时显示） |
| `agent_done` | AgentDoneEvent | `{total_steps, total_tool_calls, total_duration_ms, total_tokens, total_cost_usd}` | 岛显示完成总结 |
| `tokens_updated` | TokenStatsEvent | `{input, output, cache_read, cache_write, context_used, context_total}` | 胶囊常驻显示上下文占用 |
| `cache_diag` | CacheDiagnosticsEvent | `{prefix_changed, hit_rate, anthropic_cache_hit_tokens}` | 岛显示缓存指标 |
| `compaction_paused` | CompactionPausedEvent | `{ratio, consecutive_compacts, notice}` | 岛显示警告状态 |
| `balance_updated` | (内部生成) | `{balance_usd, used_usd, fetched_at}` | 余额刷新后推送 |
| `cost_updated` | (CostAccumulator) | `{task_cost, session_cost, is_estimated, breakdown}` | 费用增量推送 |
| `session_ended` | SessionEndEvent | `{session_id, reason}` | GUI 标记会话离线 |

**事件示例**：

```json
{"kind":"event","type":"tool_call","seq":108,"ts":1789456789.123,"data":{"call_id":"call_7","tool_name":"Bash","tool_type":"Execute","arguments":"cmake --build build --config Release","started_at":1789456789.0}}
{"kind":"event","type":"tool_result","seq":109,"ts":1789456791.456,"data":{"call_id":"call_7","tool_name":"Bash","is_error":false,"duration_ms":2345,"result_preview":"exit code 0, 构建成功（42 目标）"}}
{"kind":"event","type":"tokens_updated","seq":110,"ts":1789456791.500,"data":{"input":15234,"output":8721,"cache_read":42000,"context_used":57000,"context_total":1000000}}
```

### 4.3 请求类型 Schema（GUI → TUI）

| 请求 type | data | 响应 data | 说明 |
|-----------|------|-----------|------|
| `hello` | `{gui_version, gui_caps, last_seq}` | `{tui_version, session_id, project_root, model, provider, started_at, current_state}` | 连接握手，交换能力 |
| `refresh_balance` | `{}` | `{balance_usd, used_usd, fetched_at, source}` | 手动触发余额刷新 |
| `get_session_summary` | `{}` | `{session_id, total_steps, total_tool_calls, total_cost_usd, total_tokens, started_at}` | GUI 打开会话面板时拉取 |
| `get_model_pricing` | `{}` | `{models:[{name, input_per_1m, output_per_1m, cache_read_per_1m, cache_write_per_1m, context_window}]}` | GUI 首次加载单价表 |
| `subscribe` | `{event_types, since_seq}` | `{ok:true}` | 订阅过滤（默认订阅全部） |
| `ping` | `{}` | `{pong:true, tui_pid:12345}` | 心跳（GUI 每 5s 一次，3 次超时判定断开） |

### 4.4 重连与缓冲

- TUI 端维护**环形缓冲区**（容量 1024 条事件，约 256KB），保留最近事件的 `seq`
- GUI 断线重连时 `hello` 请求携带 `last_seq`，TUI 从缓冲区回放 `seq > last_seq` 的事件，再切到实时推送
- 缓冲区满则丢弃最旧事件，GUI 检测到 `seq` 跳变时拉取 `get_session_summary` 补全状态

### 4.5 多会话支持

TUI 进程独立，每个 TUI 在 `~/.workx/island.registry` 写一条记录：

```json
{
  "sessions": [
    {"pid":12345, "pipe":"\\\\.\\pipe\\workx-island-12345", "project_root":"D:\\develop\\Workspace\\workx", "started_at":1789456000, "model":"deepseek-v4-flash", "last_heartbeat":1789456790},
    {"pid":12999, "pipe":"\\\\.\\pipe\\workx-island-12999", "project_root":"D:\\other\\proj", "started_at":1789456500, "model":"glm-4.6", "last_heartbeat":1789456795}
  ]
}
```

- GUI 启动时扫描注册文件，列出所有活跃 TUI（pid 存活检测 + heartbeat 在 15s 内）
- 每个 TUI 一条独立连接，GUI 用 Tab 或列表切换
- 心跳每 5s 更新 `last_heartbeat`，超过 30s 视为僵尸，GUI 不再列它

### 4.6 事件流时序示例（一次工具调用）

```
TUI                                  GUI
 │                                    │
 │ session_started ────────────────► │ 初始化岛、显示会话徽标
 │                                    │
 │ thinking_started ───────────────► │ 岛扩展: ● 思考中...
 │ thinking_delta (流式) ─────────► │ 累积思考内容(可选订阅)
 │ thinking_done ──────────────────► │ 岛收起, 显示「思考 2.3s」
 │                                    │
 │ tool_call(Bash) ────────────────► │ 岛扩展: ⚡ Bash · cmake --build
 │                                    │
 │ tokens_updated ─────────────────► │ 胶囊更新: 57k/1M · $0.012
 │                                    │
 │ tool_result ────────────────────► │ 岛显示: ✓ 2.3s · 42 目标
 │                                    │
 │ balance_updated ────────────────► │ 胶囊更新: $10.478
 │                                    │
 │ agent_done ─────────────────────► │ 岛显示: 完成 3 步 · $0.034
 │                                    │
 │ ◄────────────── ping ──────────── │ 每 5s 心跳
 │ ──────────── pong ─────────────► │
```

---

## 5. GUI 状态机与岛形态渲染

### 5.1 状态机总览

岛有 5 个核心状态，按「最小打扰 → 详情」递进：

```
                    ┌──────────────────────────────────────┐
                    │                                      │
                    ▼                                      │
              ┌──────────┐    用户点击/事件发生        ┌──────────┐
   启动 ──►  │ HIDDEN   │ ───────────────────────► │ COLLAPSED│
              │ (隐藏)   │                            │ (胶囊)   │
              └──────────┘ ◄─────────────────────── └──────────┘
                    ▲           5s 静默 + 鼠标离开            │
                    │                                      │ 用户点击
                    │                                      ▼
                    │                                 ┌──────────┐
                    │           5s 静默            │ EXPANDED │
                    └───────────────────────────── │ (详情)   │
                                                  └──────────┘
                        ▲                            │
                        │                            │ 错误事件
                        │                            ▼
                        │     用户确认/5s         ┌──────────┐
                        └─────────────────────── │  ALERT   │
                                                  │ (告警)   │
                                                  └──────────┘
```

| 状态 | 触发条件 | 视觉形态 | 尺寸 (W×H) | 自动收起 |
|------|----------|----------|------------|----------|
| **HIDDEN** | 启动时 / 5s 静默后无活跃会话 | 窗口存在但 alpha=0，不响应输入 | 0×0 | — |
| **COLLAPSED** | 有活跃会话 + 非思考/工具中 | 单行胶囊（余额 · 上下文 · 任务耗时） | 380×40 | — |
| **EXPANDED** | 用户点击胶囊 / 工具调用发生 | 卡片展开（胶囊内容 + 详情区） | 380×160 | 5s 静默 |
| **ALERT** | `compaction_paused` / 工具失败 / 余额低 | 黄/红色胶囊 + 闪烁 | 380×56 | 用户确认 |
| **DETAIL** | EXPANDED 状态下点击「ⓘ」按钮 | 全屏面板（账单/历史/单价表） | 480×360 | 用户关闭 |

### 5.2 COLLAPSED 胶囊布局

```
╭──────────────────────────────────────────────────────────╮
│  🐋  $10.478  ·  ▰▰▰▱▱ 57k/1M  ·  ⚡ Bash 2.3s  ·  ⏱ 12s │
╰──────────────────────────────────────────────────────────╯
   ↑    ↑          ↑              ↑                ↑
  logo  余额      上下文条         当前活动          任务耗时
```

- **logo**：Q版鲸鱼娘头像（24×24 圆角，复用项目 docs/img/characters/）
- **余额**：`$10.478` 绿色（>5）、黄色（1-5）、红色（<1）
- **上下文条**：5 格进度条，颜色按四档水位（绿/黄/橙/红，复用 cache_aware_compactor 阈值）
- **当前活动**：状态相关，按优先级覆盖
  - 思考中：`● 思考 2.3s`（绿色脉冲点）
  - 工具调用：`⚡ Bash · cmake --build`（截断到 20 字符）
  - 空闲：`💤 空闲`（灰色）
- **任务耗时**：自 session_started 起累计；agent_done 后显示总耗时 5s 再回到胶囊

无活跃 TUI 时整个胶囊显示：`🐋 workx · 未连接 · ⌕ 查找会话`

### 5.3 EXPANDED 卡片布局

```
╭─ workx · D:\develop\Workspace\workx ──────────────── ⊟ ─╮
│  🐋 DeepSeek-V4-Flash    $10.478 (▼ $0.034 本任务)     │
│  ▰▰▰▱▱ 57,234 / 1,000,000  ·  缓存命中 73%  ·  12.3s   │
├─────────────────────────────────────────────────────────┤
│  ⚡ Bash · cmake --build build --config Release         │
│  ✓ exit code 0 · 42 目标 · 2.3s                         │
│  ┌──────────────────────────────────────────────────┐   │
│  │ [1] Read   src/main.cpp          ✓ 12ms          │   │
│  │ [2] Bash   cmake --build ...     ✓ 2.3s          │   │
│  │ [3] Edit   src/main.cpp          ✓ 8ms           │   │
│  │ [4] Bash   cmake --build ...     ⏳ 运行中        │   │
│  └──────────────────────────────────────────────────┘   │
│                                  ⓘ 详情  ·  ✕ 关闭       │
╰──────────────────────────────────────────────────────────╯
```

- 顶部：会话信息行（项目路径 + 模型 + 余额 + 本任务花费增量）
- 中部：当前活动详情（工具名+参数+结果预览，3 行截断）
- 工具调用历史列表（最近 10 条，可滚动）
- 底部：操作按钮（详情打开 DETAIL 面板）

### 5.4 DETAIL 全屏面板（ViewWindow）

点击「ⓘ」打开 DearTs 标准停靠窗口，分 3 个 Tab：

- **会话 Tab**：实时 Token 用量、花费分解、工具调用历史
- **余额历史 Tab**：折线图（implot，DearTs 已集成）显示最近 7 天余额变化
- **单价表 Tab**：DeepSeek 各模型单价（从 `get_model_pricing` 拉取或硬编码 fallback）

### 5.5 形态切换动画

```cpp
// IslandController 状态机核心
struct IslandState {
    enum Phase { HIDDEN, COLLAPSED, EXPANDED, ALERT, DETAIL };
    Phase current = HIDDEN;
    Phase target  = HIDDEN;
    float anim_t  = 0.0f;           // 0→1 动画进度
    float anim_duration = 0.25f;    // 250ms
    float idle_timer = 0.0f;        // 静默计时
};

void IslandController::update(float dt) {
    if (anim_t < 1.0f) {
        anim_t = std::min(1.0f, anim_t + dt / anim_duration);
    }
    
    idle_timer += dt;
    if (current == EXPANDED && idle_timer > 5.0f && !mouse_hover) {
        transition_to(COLLAPSED);
    }
    
    const float t = ease_out_cubic(anim_t);
    const float h = lerp(height_of(prev_phase), height_of(current), t);
    SDL_SetWindowSize(window, WIDTH, (int)h);
    
    const float alpha = (current == HIDDEN) ? lerp(prev_alpha, 0.0f, t) 
                                            : lerp(prev_alpha, 1.0f, t);
}
```

**视觉细节**：
- 圆角 12px（用 ImGui `ImDrawList::AddRectFilled`）
- 阴影：用预渲染的模糊边框纹理 9-slice 拼接
- 进入动画：从胶囊高度向上「长出」详情区，配合 0.85→1.0 缩放
- 退出动画：反向，详情区向下收缩
- ALERT 状态：胶囊边框橙色闪烁（`sin(t*8)*0.5+0.5` 调制 alpha）

### 5.6 鼠标交互

- **悬停 1s** → COLLAPSED 自动 EXPANDED（不点击）
- **鼠标离开 + 5s** → 自动收回到 COLLAPSED
- **点击岛体** → 强制 EXPANDED 并锁定（不自动收起）
- **点击外部** → 收起锁定
- **右键** → 上下文菜单（连接管理 / 设置 / 退出）
- **拖拽岛体** → 移动位置（位置持久化到 config）

### 5.7 多会话切换

```
╭─ workx · 2 个会话 ──────────────────────── ⊟ ─╮
│ [●] workx      D:\...\workx      ⚡ Bash 2.3s │  ← 当前
│ [○] blog-gen   D:\...\blog       💤 空闲      │
└────────────────────────────────────────────────┘
```

- 顶部多一行会话切换栏，每个会话一个圆点 + 项目名
- 切换时岛内容整体替换，无动画
- 未选中的会话显示为半透明，事件仍累积（badge 计数）

### 5.8 渲染层次（ImGui DrawList）

```
┌─ Layer 4: 文字内容 (标题/数值/列表)
├─ Layer 3: 进度条/图表/按钮
├─ Layer 2: 阴影 (模糊纹理 9-slice)
├─ Layer 1: 圆角背景矩形 (动态高度)
└─ Layer 0: SDL_RenderClear(alpha=0 透明)
```

---

## 6. 费用统计逻辑与余额拉取

### 6.1 费用计算总览

费用分两层独立计算：

```
┌──────────────── TUI 端（真实成本来源）─────────────────┐
│                                                       │
│  ReAct Loop ──► 每次 LLM 调用返回 usage ──► CostAccumulator
│                  {input, output,                       │
│                   cache_read, cache_write}             │
│                       │                                │
│                       ▼ 按 PricingTable 折算 USD       │
│                 session_cost_usd                       │
│                       │                                │
│                       ▼ 通过 IPC event 推送            │
│                 cost_updated / agent_done              │
└───────────────────────┬───────────────────────────────┘
                        │
                        ▼
┌──────────────── GUI 端（展示与历史）───────────────────┐
│                                                       │
│  BillingModel (累计接收的 cost events)                │
│   ├── current_task_cost  (本任务, agent_done 后清零)  │
│   ├── session_total_cost (会话累计)                   │
│   ├── daily_cost         (按日聚合)                   │
│   └── balance_history    (每次 balance_updated 追加)  │
└───────────────────────────────────────────────────────┘
```

### 6.2 PricingTable（单价表）

```cpp
// tui 侧: src/island/pricing_table.h
struct ModelPricing {
    std::string model;
    double input_per_1m         = 0.0;   // USD / 1M tokens
    double output_per_1m        = 0.0;
    double cache_read_per_1m    = 0.0;   // DeepSeek 命中缓存读单价
    double cache_write_per_1m   = 0.0;   // 写入缓存的额外单价
    int    context_window       = 0;
};

// DeepSeek 2026-04 官方定价（fallback）
static const std::vector<ModelPricing> kDeepSeekPricing = {
    {.model = "deepseek-v4-flash", .input_per_1m = 0.27, .output_per_1m = 1.10,
     .cache_read_per_1m = 0.07, .cache_write_per_1m = 0.27, .context_window = 1000000},
    {.model = "deepseek-v4-reasoner", .input_per_1m = 0.55, .output_per_1m = 2.19,
     .cache_read_per_1m = 0.14, .cache_write_per_1m = 0.55, .context_window = 1000000},
    {.model = "deepseek-chat", .input_per_1m = 0.27, .output_per_1m = 1.10,
     .cache_read_per_1m = 0.07, .cache_write_per_1m = 0.27, .context_window = 64000},
};
```

单价表加载顺序：
1. 启动时用硬编码 fallback
2. 用户配置 `~/.workx/pricing.json` 覆盖
3. GUI 首次连接时 `get_model_pricing` 拉取 TUI 端最终单价

### 6.3 CostAccumulator（TUI 端费用累积器）

```cpp
class CostAccumulator {
public:
    CostAccumulator(agent::EventBus& bus, PricingTable pricing);
    
    void on_token_stats(const agent::TokenStatsEvent& e);
    void on_agent_done(const agent::AgentDoneEvent& e);
    void on_session_start(const agent::SessionStartEvent& e);
    
    CostSnapshot get_snapshot() const;
    
private:
    struct Cost {
        double input_usd      = 0.0;
        double output_usd     = 0.0;
        double cache_read_usd = 0.0;
        double cache_write_usd= 0.0;
        double total_usd      = 0.0;
        bool   is_estimated   = false;
    };
    
    Cost m_task_cost;       // 当前任务（一个 user turn）
    Cost m_session_cost;    // 会话累计
    std::string m_current_model;
    
    Cost calc_delta(const TokenStats& usage) const;
    void publish_cost_update();
};

void CostAccumulator::on_token_stats(const agent::TokenStatsEvent& e) {
    if (e.usage.delta) {
        auto delta = calc_delta(e.usage);
        m_task_cost    += delta;
        m_session_cost += delta;
        publish_cost_update();
    }
}

void CostAccumulator::on_agent_done(const agent::AgentDoneEvent& e) {
    m_session_cost += m_task_cost;
    m_task_cost = {};
    publish_cost_update();
}

void CostAccumulator::on_session_start(const agent::SessionStartEvent& e) {
    m_session_cost = {};
    m_task_cost = {};
    m_current_model = e.model;
}
```

### 6.4 BalanceFetcher（余额拉取）

**关键约束**：
- HTTP 总超时 < 120s（项目硬约束），余额拉取用更激进的 **15s 超时**
- 复用 provider 的 api_key
- 低频拉取：启动时 1 次 + 每 10min 1 次 + 任务完成后 1 次

```cpp
class BalanceFetcher {
public:
    BalanceFetcher(agent::EventBus& bus, 
                   std::string api_key, 
                   std::string base_url = "https://api.deepseek.com");
    
    void start();
    void stop();
    BalanceResult fetch_once();
    void trigger_refresh();
    
private:
    void run_loop(const std::atomic<bool>& cancel);
    BalanceResult do_http_fetch();
    void publish_balance_event(const BalanceResult& r);
    
    agent::EventBus& m_bus;
    std::string m_api_key;
    std::string m_base_url;
    std::atomic<bool> m_refresh_requested{false};
    std::condition_variable m_cv;
    std::mutex m_mtx;
    BalanceResult m_last_result;
};

BalanceResult BalanceFetcher::do_http_fetch() {
    // GET https://api.deepseek.com/user/balance
    // Authorization: Bearer <api_key>
    // 超时 15s
    HttpClient::Request req{
        .url = m_base_url + "/user/balance",
        .method = "GET",
        .headers = {{"Authorization", "Bearer " + m_api_key}},
        .timeout_ms = 15000,   // ⚠️ 项目约束: < 120s
    };
    
    auto resp = HttpClient::get(req);
    if (!resp.ok()) {
        return {.success = false, .error = resp.error_message};
    }
    
    // {"is_available":true, "balance_infos":[{"currency":"CNY","total_balance":"10.50"}]}
    auto j = nlohmann::json::parse(resp.body);
    BalanceResult r;
    r.success = j.value("is_available", false);
    r.fetched_at = std::time(nullptr);
    
    if (r.success && !j["balance_infos"].empty()) {
        double cny = std::stod(j["balance_infos"][0].value("total_balance", "0"));
        double rate = Config::get_or("island.usd_cny_rate", 7.2);
        r.balance_usd = cny / rate;
    }
    return r;
}

void BalanceFetcher::run_loop(const std::atomic<bool>& cancel) {
    while (!cancel) {
        std::unique_lock lock(m_mtx);
        m_cv.wait_for(lock, std::chrono::minutes(10), 
                       [this] { return m_refresh_requested.load() || cancel; });
        m_refresh_requested = false;
        lock.unlock();
        
        if (cancel) break;
        
        auto result = do_http_fetch();
        if (result.success) {
            m_last_result = result;
            publish_balance_event(result);
        }
    }
}
```

**触发时机**：
1. **启动时**：TUI 初始化后立即 `trigger_refresh()`
2. **每 10 分钟**：`wait_for` 超时自动拉
3. **任务完成时**：CostAccumulator 在 `on_agent_done` 中调用 `trigger_refresh()`
4. **GUI 手动请求**：GUI 发 `refresh_balance` → TUI 调用 `trigger_refresh()` + 同步等待 3s 返回

### 6.5 GUI 端 BillingModel（展示与历史）

```cpp
class BillingModel {
public:
    void on_cost_updated(const CostUpdatedEvent& e);
    void on_balance_updated(const BalanceUpdatedEvent& e);
    void on_session_started(const SessionStartedEvent& e);
    void on_agent_done(const AgentDoneEvent& e);
    
    double current_task_cost() const { return m_task_cost.total_usd; }
    double session_total_cost() const { return m_session_cost.total_usd; }
    double current_balance() const { return m_last_balance.balance_usd; }
    
private:
    Cost m_task_cost;
    Cost m_session_cost;
    BalanceResult m_last_balance;
    std::vector<BalancePoint> m_balance_history;  // 持久化到 ~/.workx/billing.json
    std::vector<TaskCost> m_task_history;
};
```

**展示策略**：
- 胶囊显示 `current_balance()`，颜色按阈值变（>5 绿、1-5 黄、<1 红）
- EXPANDED 卡片显示 `current_task_cost()`
- DETAIL 面板用 implot 画余额历史折线图

### 6.6 数据流时序（一次完整任务）

```
用户问问题
    │
    ▼
TUI: session_started event ────────────► GUI: 初始化 BillingModel
TUI: BalanceFetcher.trigger_refresh() ──► GUI: balance_updated (初始余额 $10.512)
    │
    ▼
TUI: LLM 调用 #1
TUI: TokenStatsEvent(usage) ───────────► GUI: cost_updated (+$0.012)
TUI: thinking_done ─────────────────────► GUI: 岛显示「思考 2.3s」
    │
    ▼
TUI: tool_call(Bash) ──────────────────► GUI: 岛扩展显示工具
TUI: tool_result ──────────────────────► GUI: 岛显示「✓ 2.3s」
    │
    ▼
TUI: LLM 调用 #2 (带 cache_read)
TUI: TokenStatsEvent(usage) ───────────► GUI: cost_updated (+$0.008)
    │
    ▼
TUI: AgentDoneEvent ───────────────────► GUI: 岛显示「完成 $0.020」
TUI: CostAccumulator.on_agent_done:      GUI: task_cost 清零, task_history 追加
  - task_cost → session_cost
  - task_cost = {}
TUI: BalanceFetcher.trigger_refresh() ──► GUI: balance_updated ($10.492, ▼ $0.020)
```

---

## 7. 插件划分、目录结构与启动流程

### 7.1 插件划分

GUI 端基于 DearTs 框架，按「单一职责」拆分为 5 个内置插件 + 1 个可选托盘插件：

| 插件 | 职责 | 依赖 |
|------|------|------|
| `whale_dock_ipc` | IPC 客户端，连接 TUI，JSONL → EventBus | 无 |
| `whale_dock_billing` | 费用统计，BillingModel，持久化 | ipc |
| `whale_dock_session` | 会话管理，扫描 registry，多会话切换 | ipc |
| `whale_dock_core` | 主插件，IslandView 渲染，状态机 | ipc, billing, session |
| `whale_dock_settings` | 设置项（位置/动画/汇率/拉取间隔） | 无 |
| `whale_dock_tray` | 系统托盘（可选，平台特化） | core |

### 7.2 目录结构

**GUI 独立仓库** `D:\develop\Workspace\whale-dock\`：

```
whale-dock/
├── CMakeLists.txt
├── main/
│   └── gui/
│       ├── source/
│       │   ├── main.cpp               # 入口：创建 DearTsApplication
│       │   └── whale_dock_app.cpp   # 注册所有插件
│       └── CMakeLists.txt
├── plugins/
│   ├── whale_dock_ipc/
│   │   ├── include/
│   │   │   ├── ipc_plugin.hpp
│   │   │   ├── ipc_client.hpp
│   │   │   ├── ipc_event_converter.hpp
│   │   │   └── events.hpp
│   │   ├── source/
│   │   │   ├── ipc_plugin.cpp
│   │   │   ├── ipc_client.cpp
│   │   │   └── ipc_event_converter.cpp
│   │   └── CMakeLists.txt
│   ├── whale_dock_billing/
│   │   ├── include/
│   │   │   ├── billing_plugin.hpp
│   │   │   ├── billing_model.hpp
│   │   │   ├── pricing_table.hpp
│   │   │   └── views/billing_view.hpp
│   │   ├── source/
│   │   │   ├── billing_plugin.cpp
│   │   │   ├── billing_model.cpp
│   │   │   └── views/billing_view.cpp
│   │   └── CMakeLists.txt
│   ├── whale_dock_core/
│   │   ├── include/
│   │   │   ├── core_plugin.hpp
│   │   │   ├── island_view.hpp
│   │   │   ├── island_controller.hpp
│   │   │   ├── island_renderer.hpp
│   │   │   └── island_state.hpp
│   │   ├── source/
│   │   │   ├── core_plugin.cpp
│   │   │   ├── island_view.cpp
│   │   │   ├── island_controller.cpp
│   │   │   └── island_renderer.cpp
│   │   ├── resources/
│   │   │   └── characters/            # 鲸鱼娘头像资源
│   │   └── CMakeLists.txt
│   ├── whale_dock_session/
│   │   ├── include/
│   │   │   ├── session_plugin.hpp
│   │   │   ├── session_registry.hpp
│   │   │   └── views/session_list_view.hpp
│   │   ├── source/
│   │   │   ├── session_plugin.cpp
│   │   │   ├── session_registry.cpp
│   │   │   └── views/session_list_view.cpp
│   │   └── CMakeLists.txt
│   ├── whale_dock_settings/
│   │   ├── include/
│   │   │   ├── settings_plugin.hpp
│   │   │   └── views/settings_view.hpp
│   │   ├── source/
│   │   │   ├── settings_plugin.cpp
│   │   │   └── views/settings_view.cpp
│   │   └── CMakeLists.txt
│   └── whale_dock_tray/
│       ├── include/
│       │   └── tray_plugin.hpp
│       ├── source/
│       │   ├── tray_plugin.cpp        # 接口 + 平台检测
│       │   ├── tray_win32.cpp         # Shell_NotifyIconW
│       │   ├── tray_macos.mm          # NSStatusItem
│       │   └── tray_linux.cpp         # SNI D-Bus
│       └── CMakeLists.txt
├── resources/
│   ├── fonts/                         # OPPOSans-M.ttf
│   ├── images/
│   │   └── whale_icon.png
│   └── icon.ico / icon.icns / icon.png
└── third_party/
    └── asio/                          # header-only
```

**WorkX 主仓库新增模块** `src/island/`：

```
src/island/
├── CMakeLists.txt
├── island_server.h / .cpp             # IPC 服务端
├── island_event_bridge.h / .cpp       # EventBus → JSONL
├── cost_accumulator.h / .cpp          # 费用累积器
├── balance_fetcher.h / .cpp           # 余额拉取
├── pricing_table.h / .cpp             # 单价表
├── registry_writer.h / .cpp           # registry 文件
└── ipc/
    ├── itransport.h
    ├── transport_win32.cpp
    ├── transport_posix.cpp
    └── asio_wrapper.h
```

### 7.3 启动流程

**TUI 端启动**：

```
main.cpp
  ├── 1. 初始化 agent::EventBus、ConfigManager
  ├── 2. 创建 IBackend (DeepSeek provider)
  ├── 3. 初始化 IslandServer
  │     ├── 创建 IPC 端点
  │     ├── RegistryWriter 写 ~/.workx/island.registry
  │     └── 启动 IPC accept 线程
  ├── 4. 初始化 CostAccumulator (订阅 EventBus)
  ├── 5. 初始化 BalanceFetcher
  │     ├── 从 provider_config 取 api_key
  │     ├── 启动后台拉取任务
  │     └── trigger_refresh() 拉第一次
  ├── 6. IslandEventBridge 订阅 EventBus → JSONL 推送
  ├── 7. 启动 TUI 主循环
  └── 退出时:
        ├── 广播 session_ended
        ├── 移除 registry 记录
        └── 关闭所有 IPC 连接
```

**GUI 端启动**：

```
main.cpp
  ├── 1. 创建 DearTsApplication
  ├── 2. 注册插件 (按依赖顺序)
  ├── 3. Application::initialize()
  │     ├── 创建 SDL3 透明置顶窗口 (380×40)
  │     ├── 加载中文字体
  │     └── ImGui 初始化
  └── 4. Application::run() 主循环
        ├── IpcPlugin: 扫描 registry → 连接 TUI → 读 JSONL
        ├── CorePlugin: IslandController 状态机 + 渲染
        ├── BillingPlugin: 订阅事件 + 更新模型
        └── SDL_RenderPresent (60fps)
```

### 7.4 关键代码骨架

**TUI 端 main.cpp 接线**：

```cpp
int main(int argc, char* argv[]) {
    auto& bus = agent::EventBus::instance();
    auto config = load_config();
    
    // ... 原有初始化 ...
    
    // 新增: Island 模块初始化
    island::IslandServer island_server(bus);
    island_server.start();
    
    island::CostAccumulator cost_acc(bus, island::load_pricing_table(config));
    
    auto backend = factory::create_backend(config);
    island::BalanceFetcher balance_fetcher(bus, 
                                            backend->api_key(), 
                                            backend->base_url());
    balance_fetcher.start();
    
    run_tui_main_loop(bus, backend, cost_acc);
    
    balance_fetcher.stop();
    island_server.stop();
    return 0;
}
```

**GUI 端 main.cpp**：

```cpp
class WhaleDockApp : public DearTs::Core::Application {
public:
    void setup_plugins() override {
        auto& pm = PluginManager::instance();
        pm.add_builtin(std::make_unique<IpcPlugin>());
        pm.add_builtin(std::make_unique<BillingPlugin>());
        pm.add_builtin(std::make_unique<SessionPlugin>());
        pm.add_builtin(std::make_unique<CorePlugin>());
        pm.add_builtin(std::make_unique<SettingsPlugin>());
        if (TrayPlugin::is_platform_supported()) {
            pm.add_builtin(std::make_unique<TrayPlugin>());
        }
    }
    
    void on_init() override {
        SDL_PropertiesID props = SDL_CreateProperties();
        SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "WhaleDock");
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, 380);
        SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, 40);
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_TRANSPARENT_BOOLEAN, true);
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_BORDERLESS_BOOLEAN, true);
        SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_ALWAYS_ON_TOP_BOOLEAN, true);
        m_window = SDL_CreateWindowWithProperties(props);
        SDL_DestroyProperties(props);
        
        m_renderer = SDL_CreateRenderer(m_window, nullptr);
        ImGui_ImplSDL3_InitForSDLRenderer(m_window, m_renderer);
        ImGui_ImplSDLRenderer3_Init(m_renderer);
    }
};
```

### 7.5 构建与分发

| 平台 | 产物 | 说明 |
|------|------|------|
| Windows | `WhaleDock.exe` + `resources/` | 单目录分发，WIN32_EXECUTABLE |
| macOS | `WhaleDock.app` bundle | LSUIElement=true，签名 + 公证 |
| Linux | AppImage | 含 AppStream metadata |

---

## 8. 错误处理与测试策略

### 8.1 错误处理矩阵

| 故障域 | 故障场景 | 严重度 | 处理策略 |
|--------|----------|--------|----------|
| IPC 传输 | TUI 未启动 | 预期 | 岛显示「未连接 · ⌕ 查找会话」 |
| IPC 传输 | 连接中断 | 预期 | 标记离线 + 指数退避重连（1s/2s/4s/8s/16s/30s 上限） |
| IPC 传输 | registry 文件损坏 | 预期 | 跳过该条目，日志 WARN |
| IPC 协议 | JSON 解析失败 | 中 | 丢弃该行，连续 10 次失败断开重连 |
| IPC 协议 | seq 跳变 | 中 | `get_session_summary` 补全状态 |
| IPC 协议 | 请求超时（5s） | 中 | 标记 TUI「无响应」，30s 后重试 |
| 余额 API | HTTP 超时 | 预期 | 保留上次值，下次 10min 重试 |
| 余额 API | 401 未授权 | 高 | 停止拉取，显示「API Key 无效」 |
| 余额 API | 余额 < $1 | 中 | 岛进入 ALERT 状态，红色闪烁 |
| 费用统计 | 模型名未匹配 | 中 | 按 deepseek-chat fallback + 标记「≈」 |
| 费用统计 | usage 数据缺失 | 中 | 跳过该事件，仅日志 |
| GUI 渲染 | 透明窗口不支持 | 中 | 降级为不透明圆角窗口 |
| 系统托盘 | 平台不支持 SNI | 低 | 不加载托盘，快捷键唤起 |
| 持久化 | billing.json 写入失败 | 低 | 跳过，内存数据保留 |

### 8.2 重连状态机

```cpp
enum class ReconnectState {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    BACKING_OFF,
    FAILED_PERMANENT,  // 连续 10 次失败
};

int next_backoff_ms(int attempt) {
    int base = std::min(1000 * (1 << attempt), 30000);
    int jitter = rand() % (base / 4);
    return base + jitter;
}
```

### 8.3 测试策略

**1. 单元测试（GoogleTest，CI 必须）**

WorkX 主仓库 `tests/unit/island/`：

```
test_pricing_table.cpp        # 单价表查找/匹配/fallback
test_cost_accumulator.cpp     # 费用累积/任务清零/会话清零
test_balance_fetcher.cpp      # HTTP mock/超时/401/解析
test_ipc_transport.cpp        # Win pipe / POSIX socket 读写
test_island_event_bridge.cpp  # EventBus → JSONL 序列化
test_registry_writer.cpp      # registry 文件读写/pid 存活检测
test_jsonl_protocol.cpp       # 消息编解码/seq 排序/请求响应关联
```

GUI 仓库 `tests/unit/`：

```
test_billing_model.cpp        # 费用累加/持久化/历史聚合
test_island_controller.cpp    # 状态机转换/动画计时/自动收起
test_session_registry.cpp     # 多会话扫描/存活检测
test_ipc_client.cpp           # 重连/心跳/请求超时
test_ipc_event_converter.cpp  # JSONL → EventBus 事件映射
```

关键测试用例示例：

```cpp
TEST(CostAccumulator, TaskCostClearsOnAgentDone) {
    EventBus bus;
    PricingTable pricing{{"deepseek-v4-flash", {.input_per_1m=0.27, ...}}};
    CostAccumulator acc(bus, pricing);
    
    bus.publish(SessionStartEvent{.model = "deepseek-v4-flash"});
    bus.publish(TokenStatsEvent{.usage = {.input=1000000, .output=0, ...}});
    
    EXPECT_NEAR(acc.task_cost().total_usd, 0.27, 0.001);
    
    bus.publish(AgentDoneEvent{});
    
    EXPECT_NEAR(acc.task_cost().total_usd, 0.0, 0.001);
    EXPECT_NEAR(acc.session_cost().total_usd, 0.27, 0.001);
}

TEST(IslandController, AutoCollapseAfter5s) {
    IslandController c;
    c.transition_to(EXPANDED);
    c.update(4.9f, /*mouse_hover=*/false);
    EXPECT_EQ(c.current_phase(), EXPANDED);
    c.update(0.2f, /*mouse_hover=*/false);
    EXPECT_EQ(c.current_phase(), COLLAPSED);
}

TEST(BalanceFetcher, HttpTimeoutUnder15s) {
    BalanceFetcher f(bus, "fake_key");
    auto req = f.build_request();
    EXPECT_LE(req.timeout_ms, 15000);
}
```

**2. 集成测试**

```cpp
TEST(IslandIpc, FullEventFlowEndToEnd) {
    EventBus tui_bus;
    IslandServer server(tui_bus, ":memory:");
    server.start();
    
    IpcClient client;
    ASSERT_TRUE(client.connect(server.endpoint()));
    
    auto hello_resp = client.request("hello", {});
    ASSERT_TRUE(hello_resp.ok);
    
    tui_bus.publish(ToolCallEvent{.tool_name="Bash"});
    
    auto events = client.drain_events(/*timeout_ms=*/500);
    ASSERT_FALSE(events.empty());
    EXPECT_EQ(events[0]["type"], "tool_call");
}

TEST(IslandIpc, ReconnectReplaysFromLastSeq) {
    // 验证断线重连后从 last_seq 回放
    // ...
}
```

**3. 手动验收 checklist**

```
基础连接
  [ ] TUI 启动后 GUI 自动连接
  [ ] TUI 退出后 GUI 显示「已断开」并重连
  [ ] GUI 先启动, TUI 后启动, 自动连接
  [ ] 多个 TUI 同时运行, GUI Tab 切换
费用统计
  [ ] 单次任务花费正确累加
  [ ] agent_done 后任务花费清零, 会话花费保留
  [ ] 余额 10min 自动刷新
  [ ] 任务完成后余额即时刷新
  [ ] api_key 错误时显示「API Key 无效」
状态机
  [ ] 思考中岛显示「● 思考 Ns」
  [ ] 工具调用岛自动扩展
  [ ] 5s 静默后自动收起
  [ ] 点击胶囊锁定展开
  [ ] 余额低时红色闪烁
跨平台
  [ ] Windows 透明窗口 + 托盘
  [ ] macOS 无 Dock 图标 + 菜单栏
  [ ] Linux X11 透明窗口
```

---

## 9. 里程碑与风险

### 9.1 里程碑

**M1 — IPC 骨架打通（最小闭环）**
- WorkX: `src/island/ipc/` 传输层（Win pipe + POSIX socket）
- WorkX: `IslandServer` + `IslandEventBridge`
- WorkX: `RegistryWriter`
- GUI: `IpcPlugin` + `IpcClient`
- GUI: 最小 `DearTsApplication`（SDL3 透明窗口 + ImGui）
- 验证：TUI 启动一个工具调用 → GUI 控制台打印 JSONL

**M2 — 灵动岛渲染与状态机**
- GUI: `CorePlugin` + `IslandView` + `IslandController` + `IslandRenderer`
- GUI: 鼠标交互（悬停展开、点击锁定、5s 收起）
- 验证：岛随 TUI 事件实时变化形态

**M3 — 费用统计与余额**
- WorkX: `CostAccumulator` + `PricingTable` + `BalanceFetcher`
- GUI: `BillingPlugin` + `BillingModel` + `BillingView`（3 Tab）
- 验证：胶囊显示余额与花费，DETAIL 面板显示完整账单

**M4 — 多会话与托盘**
- GUI: `SessionPlugin` + `TrayPlugin`（三平台）+ `SettingsPlugin`
- 验证：同时连接 2 个 TUI，托盘菜单可用

**M5 — 跨平台打磨与分发**
- macOS: `.app` bundle + 签名
- Linux: AppImage
- Windows: DPI aware + 图标
- Wayland 降级测试
- 完整验收 checklist

### 9.2 风险与缓解

| 风险 | 概率 | 影响 | 缓解 |
|------|------|------|------|
| DeepSeek 余额 API 变更 | 中 | 中 | 解析容错 + fallback 到上次值 |
| Wayland 透明窗口不支持 | 高 | 中 | 降级为不透明圆角窗口 |
| Linux 托盘 SNI 兼容性 | 中 | 低 | 托盘可选，降级快捷键唤起 |
| IPC 缓冲区溢出丢事件 | 低 | 中 | 1024 条环形缓冲 + seq 跳变检测 |
| DearTs 框架 API 变更 | 低 | 高 | 固定版本，升级前回归测试 |
| asio 引入编译依赖 | 低 | 低 | header-only，无链接依赖 |

### 9.3 性能预算

| 指标 | 目标 | 说明 |
|------|------|------|
| GUI 内存占用 | < 80MB | SDL3 + ImGui + 字体 + 缓冲 |
| GUI CPU（空闲） | < 1% | 60fps 但空帧几乎无绘制 |
| GUI CPU（动画中） | < 5% | 状态转换期间 |
| IPC 延迟 | < 16ms | 每帧检查一次可读 |
| IPC 吞吐峰值 | 100 events/s | thinking_delta 流式时 |
| 余额 API 调用频率 | < 10次/小时 | 10min 间隔 + 任务完成触发 |
| billing.json 落盘 | 每 30s | 避免频繁 IO |

---

## 附录 A：与 WorkX 主仓库的关系

- WorkX 主仓库新增 `src/island/` 模块（IPC 服务端 + 费用累积），编译进 `workx.exe`
- `whale-dock` 是独立仓库，依赖 DearTs 框架，构建出独立 GUI 可执行文件
- 两者通过 `~/.workx/island.registry` 发现彼此，无编译时依赖

## 附录 B：配置项

| 配置键 | 默认值 | 说明 |
|--------|--------|------|
| `island.auto_connect` | `true` | GUI 自动连接扫描到的 TUI |
| `island.heartbeat_interval_sec` | `5` | 心跳间隔 |
| `island.idle_collapse_sec` | `5` | 静默后自动收起秒数 |
| `island.anim_duration_ms` | `250` | 状态切换动画时长 |
| `island.balance_refresh_interval_min` | `10` | 余额拉取间隔 |
| `island.usd_cny_rate` | `7.2` | CNY → USD 汇率 |
| `island.window_x` | 右上角 | 岛窗口 X 坐标 |
| `island.window_y` | `24` | 岛窗口 Y 坐标 |
| `island.balance_warn_threshold` | `5.0` | 余额警告阈值（USD） |
| `island.balance_danger_threshold` | `1.0` | 余额危险阈值（USD） |
