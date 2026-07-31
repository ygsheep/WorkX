# compact/ — 上下文压缩与缓存感知模块

> **项目**：DS_CACHE_OPTIMIZATION_PLAN
> **目标**：在长对话中维持 DeepSeek 硬盘缓存的高命中率，同时避免上下文窗口撑爆
> **设计参考**：`example/DeepSeek-Reasonix/internal/agent/`（cache_shape.go / compact.go / prune.go）

---

## 一、模块总览

```
compact/
├── token_count.h / .cpp           # Token 估算（启发式，对齐 claude-code）
├── sha256.h / .cpp                # 手写 SHA-256（FIPS 180-4，L-4：替代 std::hash）
├── prefix_shape.h / .cpp          # 前缀形状追踪与缓存劣化归因（P0 可观测性）
├── cache_aware_compactor.h / .cpp # 缓存感知分级压缩器（P1 核心）
├── tool_result_maintainer.h/.cpp  # tool_result 两级维护：snip + prune（P1）
├── context_compressor.h / .cpp    # [DEPRECATED] 旧压缩器，缓存杀手，仅保留兼容
└── truncator.h                    # 早期占位 stub，未实现
```

---

## 二、核心设计原则

DeepSeek 硬盘缓存基于**前缀字节级匹配**，任何对历史消息的非追加修改都会击穿缓存。
本模块的核心目标：**把 append-only 从"偶然正确"变为 enforced 不变量**。

| 原则 | 实现 |
|---|---|
| 前缀字节级稳定 | `pinned_prefix_len()` 钉住首条 user + 已有摘要，永不折叠 |
| 历史只追加 | 压缩只改中段，钉住前缀 + 尾部预算不变 |
| 分级触发 | soft → snip → compact → force，从轻到重 |
| 卡死守卫 | 连续 compact 仍超阈值则暂停，让前缀重新 append-only |
| 零 LLM 优先 | snip/prune 机械截短，推迟昂贵的摘要调用 |

---

## 三、模块详解

### 3.1 token_count — Token 估算

启发式估算，无外部 tokenizer 依赖（对齐 claude-code `tokenEstimation.ts`）：

- 默认 `chars / 4`（英文/代码）
- JSON 类内容 `chars / 2`（更紧凑）
- 图片/PDF 固定 2000 tokens
- 每条消息附加 4 tokens 对话结构开销

**关键函数**：
- `estimate_message_tokens(msg)` — 单条消息 token 数
- `estimate_messages_tokens(messages)` — 消息序列总 token 数

---

### 3.2 prefix_shape — 前缀形状追踪（P0 可观测性）

对 `system prompt + tools schema` 计算稳定 hash，每轮对比形状，
当缓存命中率劣化时输出归因。

**数据结构**：
```cpp
struct PrefixShape {
    std::string system_hash;     // sha256(system_prompt) 前 8 字节的十六进制（L-4）
    std::string tools_hash;      // sha256(规范化 tools schema) 前 8 字节的十六进制
    std::string prefix_hash;     // system_hash + "|" + tools_hash 联合指纹
    int rewrite_version = 0;     // 历史改写版本号
};
```

**归因规则**：
- `system_hash` 变化 → reasons 含 `"system"`
- `tools_hash` 变化 → reasons 含 `"tools"`
- `rewrite_version` 变化 → reasons 含 `"log_rewrite"`
- 全部未变但 miss 显著 → 非前缀因素（如缓存过期）

**SHA-256 实现（L-4）**：`system_hash` / `tools_hash` 使用手写 SHA-256
（[sha256.cpp](file:///d:/develop/Workspace/workx/src/agent/compact/sha256.cpp)，FIPS 180-4），
取前 8 字节（16 位十六进制）。跨编译器确定、跨进程稳定，可由 Python
`hashlib.sha256(...).hexdigest()[:16]` 外部复算。替代了早期基于 `std::hash` 的实现
（`std::hash` 实现定义，MSVC/GCC 结果不同，无法跨进程对比）。

**rewrite_version 透传（H-2）**：
- `ReActResult` 新增 `int32_t rewrite_version`，`run()` 末尾回填 `m_compactor.rewrite_version()`
- `ChatSession::run_completion` 在 `run()` 返回后二次捕获 `cur_shape`，传入 `react_result.rewrite_version`
- 使 `compare_shape` 在 compact 改写历史中段后能正确触发 `log_rewrite` 归因

tools schema 规范化：按 `function.name` 排序后序列化（`normalize_tools_schema()`），消除注册顺序抖动。
该函数同时用于 `capture_shape` 的 hash 计算和 `build_request` 的 tools 字段赋值（M-2），
保证发送给 API 的字节与 hash 计算字节一致。

**只观测，不改行为**。诊断结果通过 `CacheDiagnosticsEvent` 发布到 EventBus。

---

### 3.3 cache_aware_compactor — 缓存感知分级压缩器（P1 核心）

替代 `ContextCompressor` 的死代码实现。四档水位：

| 水位 | 占窗口 | 动作 | 对前缀的影响 |
|---|---|---|---|
| soft | 50% | 仅发 Notice，不动前缀 | 零破坏 |
| snip | 60% | 截短旧 tool_result（机械） | 改写中段，头尾不动 |
| compact | 80% | 摘要中段，钉住头+尾 | 头尾不变，中段折叠 |
| force | 90% | 强制折叠低价值区 | 同上 |

**核心方法**：

```cpp
// 主入口：在 run() 的 iteration == 1 时调用（M-6：turn 间压缩）
Result maybe_compact(std::vector<ChatMessage>& messages);

// 钉住前缀：首条 user（若 < min(1500, 窗口×15%)）+ 已有 <compaction-summary>
size_t pinned_prefix_len(const std::vector<ChatMessage>& messages) const;

// 尾部保留：从尾部按 token 预算回溯，对齐到非 tool 消息
size_t tail_start(const std::vector<ChatMessage>& messages) const;

// 中段折叠：摘要 [pinned_end, tail_start) 区间，插入 <compaction-summary>
// M-1：折叠前归档原消息到 <archive_dir>/<timestamp>.jsonl
// M-4：优先调用 m_summarize_fn（LLM 摘要），失败 fallback 到 mechanical_fold_summary
int compact_middle(std::vector<ChatMessage>& messages,
                   size_t pinned_end, size_t tail_start_idx);

// H-4：从 provider preset 注入真实窗口大小
void set_context_window(int32_t context_window_tokens);

// M-4：注入 LLM 摘要回调（失败自动 fallback 到机械折叠）
void set_summarize_fn(SummarizeFn fn);

// H-3：注册卡死守卫触发/恢复回调（发布 CompactionPausedEvent）
void set_paused_callback(PausedCallback cb);

// M-5：重置卡死状态/rewrite_version（clear_history 时调用）
void reset();
```

**context_window 注入（H-4）**：`factory.cpp::create_session` 读取
`cfg.CONTEXT_LENGTH` 或 `preset.default_context_length`，通过
`ChatSession::set_compactor_context_window()` 注入。优先级：
`cfg.backend.context_length` > `preset.default_context_length` > 0（内部 fallback 1M）。
避免小窗口模型（如 8192）因默认 1M 导致压缩永不触发。

**卡死守卫与自愈（H-3）**：连续 compact 达 `max_consecutive_compacts=2` 时置 `m_stuck=true`，
通过 `PausedCallback` 发布 `CompactionPausedEvent{paused=true}` 到 EventBus。
**自愈路径**：当 `ratio` 回落到 `soft_ratio` 以下时清除 `m_stuck`、重置 `m_consecutive_compacts`，
发布 `CompactionPausedEvent{paused=false}` 恢复事件，让前缀重新 append-only 增长恢复缓存命中后自动恢复压缩。

**归档（M-1）**：`compact_middle` 折叠前将中段消息序列化追加到
`<archive_dir>/<timestamp>.jsonl`。`archive_dir` 由
`ChatSession::set_compactor_archive_dir()` 注入（factory 从 `save_path` 派生）。
摘要占位符中标注归档路径，保证原消息可追溯。

**LLM 摘要回调（M-4）**：`ChatSession::summarize_with_llm()` 同步调用
`m_provider->submit_completion` 生成结构化摘要（system prompt 指示压缩为结构化摘要，
低温度 0.3，max_tokens=1024）。失败（null/Error/Cancelled/空）抛异常，
由 `compact_middle` 的 try/catch 自动 fallback 到 `mechanical_fold_summary`。
未注入 `summarize_fn` 时默认走机械折叠（P1 baseline）。

**会话重置（M-5）**：`ChatSession::clear_history()` 调用 `m_compactor.reset()`，
清除 `m_stuck` / `m_consecutive_compacts` / `m_rewrite_version`，避免跨会话泄漏。

**压缩点（M-6）**：压缩在 `run()` 的 `iteration == 1` 执行（turn 间压缩），
而非 `iteration > 1`（turn 内每轮），避免单 turn 多步推理链被过早截短。

---

### 3.4 tool_result_maintainer — tool_result 两级维护（P1）

在昂贵的 LLM 摘要之前，用机械方式释放空间。**不调用 LLM，零额外成本**。

**两级策略**：

| 级别 | 行为 | 占位符 | 适用水位 |
|---|---|---|---|
| snip | 保留头/尾行，中段截短 | `[snipped tool result — X lines elided]` | snip 水位 |
| prune | 整段替换为占位符 | `[elided tool result — X bytes]` | force 水位 |

**按工具语义分类策略**：

| 工具类型 | 代表工具 | 策略 | 理由 |
|---|---|---|---|
| 只读 | Read/Glob/Grep/LS/WebFetch | 头长尾短 `{80, 12}` | 答案通常在前部 |
| 副作用 | Write/Edit/Bash/PowerShell | 头尾均等 `{40, 40}` | 错误信息可能在尾部 |

**不变量**：
- 不删除消息（保持消息数不变）
- 不改 `tool_call_id`（保持 tool/assistant 配对）
- 不改 assistant content（保持前缀稳定）
- 已截短的消息不重复截短（检测 `[snipped` / `[elided` 标记）

---

### 3.5 context_compressor — [DEPRECATED]

旧压缩器实现，**缓存杀手**，已标记 `[[deprecated]]`：

- `result.erase(result.begin())` 删头部 → 前缀彻底失效
- `summarize_tool_result` 原地改写旧 tool_result → 中段前缀失效
- 阈值 `max_messages=50` / `max_tokens=8000` 与真实窗口（DeepSeek 1M）脱节

**仅保留以兼容历史测试，不在 ReActLoop 中使用**。新代码应使用 `CacheAwareCompactor`。

---

### 3.6 truncator — 早期 stub

仅含占位注释，未实现。保留以记录早期设计意图。

---

## 四、与 ReActLoop 的集成

```cpp
// react_loop.cpp 的 run() 循环中
for (int iteration = 1; iteration <= max_iterations; ++iteration) {
    // DS_CACHE M-6：turn 间压缩 — 仅 iteration == 1 时执行
    // 原 iteration > 1 会在单 turn 多步推理中过早截短前序 tool_result
    if (iteration == 1) {
        auto compact_result = m_compactor.maybe_compact(messages);
        // ... 日志记录动作与 token 变化
    }
    // M-2：build_request 中对 tools_schema 按 function.name 排序后赋值
    CompletionRequest request = build_request(messages, system_prompt, tools_schema);
    // ...
}

// run() 末尾（H-2）：
result.rewrite_version = m_compactor.rewrite_version();
// ChatSession 据此二次捕获 cur_shape，使 compare_shape 的 log_rewrite 归因生效
```

**配置入口**：`ReActLoop::Config::compactor_cfg`（类型 `CacheAwareCompactor::Config`）

**ChatSession 集成**：
- 构造时注入 `set_paused_callback`（发布 `CompactionPausedEvent`）
- 构造时注入 `set_summarize_fn`（绑定 `summarize_with_llm`）
- `factory.cpp` 注入 `set_compactor_context_window`（H-4）与 `set_compactor_archive_dir`（M-1）
- `clear_history()` 调用 `m_compactor.reset()`（M-5）

---

## 五、数据流

```
用户消息追加
      ↓
ReActLoop::run() iteration 1（M-6：turn 间压缩）
      ↓
m_compactor.maybe_compact(messages)
      ├─ 估算 tokens
      ├─ 计算水位 ratio
      ├─ soft  → Notice（不改消息）
      ├─ snip  → snip_stale_tool_results()（机械截短中段 tool_result）
      ├─ compact → compact_middle()
      │           ├─ M-1：归档原消息到 <archive_dir>/<timestamp>.jsonl
      │           ├─ M-4：调用 summarize_fn（LLM 摘要），失败 fallback 到机械折叠
      │           └─ H-3：连续 compact 达阈值 → PausedCallback 发布 CompactionPausedEvent
      └─ force → 同 compact + 标记 Force
      ↓
build_request（M-2：tools_schema 按 name 排序后赋值）
      ↓
provider->submit_completion(request)
      ↓
解析 usage.prompt_cache_hit_tokens / prompt_cache_miss_tokens
      ↓
run() 末尾回填 result.rewrite_version（H-2）
      ↓
ChatSession 二次捕获 cur_shape（传入 rewrite_version）
      ↓
prefix_shape.compare_shape(prev, cur, hit, miss)
      ├─ 发布 CacheDiagnosticsEvent（前缀变化归因，含 log_rewrite）
      └─ 更新 m_last_prefix_shape
      ↓
StreamDoneEvent 携带 hit/miss tokens
      ↓
token_stats_model 累加（M-3：TUI 侧累计，非 ChatSession）
      ↓
UI StatusBar 显示 "cache 87% (session)"
```

---

## 六、验收标准（对齐 plan/DS_CACHE_OPTIMIZATION_PLAN.md）

### P0 验收（已实现）

- [x] DeepSeek `prompt_cache_hit_tokens` / `prompt_cache_miss_tokens` 被正确解析
- [x] UI 状态栏显示会话级累计命中率
- [x] `CacheDiagnosticsEvent` 在 system/tools 变化时发布，UI 可见归因
- [x] tools schema 按 name 排序后序列化，hash 纳入前缀形状
- [x] 现有 Anthropic 路径不受影响
- [x] **M-2**：`build_request` 中对 tools schema 按 `function.name` 排序后再赋值
- [x] **L-4**：前缀形状 hash 使用手写 SHA-256（FIPS 180-4），替代 `std::hash`

### P1 验收（已实现）

- [x] 长对话在 DeepSeek 1M 窗口下分级压缩不撑爆
- [x] 压缩触发后钉住前缀 + 尾部预算，命中率不崩塌
- [x] 卡死守卫在连续 compact 达阈值时触发 Stuck 暂停
- [x] tool_result snip 机械截短释放空间（零 LLM 调用）
- [x] `rewrite_version` 追踪历史改写，与前缀形状诊断集成
- [x] **H-1**：补齐 compact 模块单元测试（33 个用例，含 SHA-256 NIST 官方向量）
- [x] **H-2**：`rewrite_version` 透传链路打通 — `ReActResult` 携带，`run()` 末尾回填，`ChatSession` 二次捕获传入
- [x] **H-3**：卡死守卫新增 `CompactionPausedEvent` + 自愈路径（ratio 回落 soft 以下清除 `m_stuck`）
- [x] **H-4**：`context_window_tokens` 从 provider preset 注入（`factory.cpp` 读取 `cfg.CONTEXT_LENGTH` 或 `preset.default_context_length`）
- [x] **M-1**：压缩归档落地 — `compact_middle` 折叠前归档到 `<archive_dir>/<timestamp>.jsonl`
- [x] **M-3**：删除 `ChatSession::session_cache_stats()` 死代码，统一由 `token_stats_model` 累计
- [x] **M-4**：LLM 摘要回调注入 — `summarize_with_llm()` 同步调用 `m_provider`，失败 fallback 到机械折叠
- [x] **M-5**：`clear_history()` 调用 `m_compactor.reset()` 重置压缩器状态
- [x] **M-6**：压缩点移至 turn 间（`iteration == 1`），避免单 turn 内多步推理链被过早截短
- [x] **L-1**：移除 `cache_aware_compactor.cpp` 恒真冗余条件
- [x] **L-2**：`chat_renderer.cpp` 的 `EventToken` 订阅统一改用 `std::make_unique`

### P2 验收（已实现）— reasoning_content 可选往返

- [x] `backend.send_reasoning_content` 配置项（默认 false，保持向后兼容）
- [x] `OpenAIAdapter` 支持条件序列化 `reasoning_content` 字段
- [x] 配置链路：`config.json` → `BackendConfig` → `OpenAIAdapter::set_send_reasoning_content()`
- [x] 仅 assistant 角色 + 非空 reasoning_content 时发送
- [x] 非 DeepSeek 模型（Gemma 等）不受影响（默认关闭）
- [x] 单元测试覆盖：默认不发送 / 开启后发送 / 空内容不发送 / 非 assistant 角色不发送

**A/B 测试方法**：在 `config.json` 中设置 `"backend.send_reasoning_content": true`，
观察 UI 状态栏的会话级缓存命中率变化与 prompt token 增量，测量净收益。

---

## 七、参考实现

- [cache_shape.go](file:///d:/develop/Workspace/workx/example/DeepSeek-Reasonix/internal/agent/cache_shape.go) — 前缀形状捕获与对比
- [compact.go](file:///d:/develop/Workspace/workx/example/DeepSeek-Reasonix/internal/agent/compact.go) — 分级压缩主逻辑
- [prune.go](file:///d:/develop/Workspace/workx/example/DeepSeek-Reasonix/internal/agent/prune.go) — tool_result 两级维护
- [run_loop.go](file:///d:/develop/Workspace/workx/example/DeepSeek-Reasonix/internal/agent/run_loop.go) — 主循环中的缓存钩子

完整设计文档：[plan/DS_CACHE_OPTIMIZATION_PLAN.md](file:///d:/develop/Workspace/workx/plan/DS_CACHE_OPTIMIZATION_PLAN.md)
