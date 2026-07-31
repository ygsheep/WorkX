# DeepSeek 上下文硬盘缓存优化方案

> **项目代号**：DS_CACHE_OPTIMIZATION_PLAN
> **目标**：让 DeepSeek API 的上下文硬盘缓存命中率可观测、可维持、可恢复
> **覆盖问题**：缓存字段未解析 / 前缀抖动无防护 / 压缩策略缓存杀手 / 无诊断归因
> **文档版本**：1.0（初稿）
> **基于代码版本**：2026-07-31
> **参考实现**：`example/DeepSeek-Reasonix/internal/agent/`（cache_shape.go / compact.go / prune.go / run_loop.go）

---

## 一、背景

### 1.1 DeepSeek 硬盘缓存机制要点

DeepSeek API 对所有用户默认开启上下文硬盘缓存，**无需修改代码**。命中率取决于客户端是否保持请求前缀的字节级稳定：

- **落盘时机**：请求结束位置、公共前缀检测、固定 token 间隔
- **命中规则**：后续请求必须**完整匹配**已落盘的缓存前缀单元（受 Sliding Window Attention 影响，非连续匹配）
- **返回字段**：`usage.prompt_cache_hit_tokens` / `usage.prompt_cache_miss_tokens`
- **输出随机性**：缓存只作用于前缀，输出仍受 `temperature` 影响

关键推论：**任何对历史消息的非追加修改（删除头部、改写中段、重排 tools schema）都会击穿缓存**。

### 1.2 参考实现 Reasonix 的命中率归因

Reasonix 命中率 >90% 是**架构设计**的产物，不是调参。核心是 6 个互锁机制：

| 机制 | 实现位置 | 作用 |
|---|---|---|
| 前缀形状追踪 | `cache_shape.go` | 让缓存劣化可观测归因（system/tools/log_rewrite） |
| 历史只追加 | `run_loop.go:235` | 每轮 `session.Add` 不改既有消息，前缀 100% 命中 |
| 缓存感知分级压缩 | `compact.go` | 4 档水位，每档把"是否破坏前缀"作为第一决策 |
| 卡死守卫 | `compact.go:141` | 窗口太小连续压缩时暂停，让前缀重新 append-only |
| tool_result 两级维护 | `prune.go` | 无 API 调用机械截短，推迟昂贵摘要 |
| reasoning 往返成本量化 | `cachehit_e2e_test.go` | 显式测量 CoT 往返对命中率的代价 |

**核心铁律**：第 N 次请求 = 第 N-1 次请求的完整前缀 + 新尾部。`TestCacheHitPrefixStable` 用 `hitChars[i] == reqChars[i-1]` 直接断言。

### 1.3 Reasonix 分级压缩水位（对齐参考）

| 水位 | 占窗口 | 动作 | 对前缀的影响 |
|---|---|---|---|
| soft | 50% | 仅发 Notice，不动前缀 | 零破坏 |
| snip | 60% | `SnipStaleToolResults` 截短旧 tool_result | 改写中段，头尾不动 |
| compact | 80% | 摘要中段，钉住头+尾 | 头尾不变，中段折叠 |
| force | 90% | 强制折叠低价值区 | 同上 |

关键设计：
- `pinnedPrefixLen` 钉住 `system + 第一条 user（若小）+ 已有摘要`，永不折叠
- `tailStart` 用 **token 预算**（非消息条数）计算尾部边界，对齐到非 tool 消息避免孤儿
- `partitionFold` 区分 `kept`（用户事实 + 摘要）与 `fold`（可折叠的 assistant/tool 工作）
- 摘要包在 `<compaction-summary>` 标签内，原消息归档到 `.jsonl` 保证可追溯

---

## 二、workx 现状诊断

对照 Reasonix 6 点，[src/agent/](file:///d:/develop/Workspace/workx/src/agent) 当前存在 **5 个关键缺口**：

### 缺口 1：ContextCompressor 是死代码

- [react_loop.h:318](file:///d:/develop/Workspace/workx/src/agent/core/react_loop.h#L318) 声明 `m_compressor` 成员
- [react_loop.cpp:38](file:///d:/develop/Workspace/workx/src/agent/core/react_loop.cpp#L38) 初始化它
- 但 `run()` 和 `build_request()` **从未调用** `compress()`

**当前实际行为**：纯 append-only（命中率天然高），但长对话会撑爆窗口，且无任何防护。

### 缺口 2：现有压缩策略若启用会摧毁缓存

[context_compressor.cpp:53](file:///d:/develop/Workspace/workx/src/agent/compact/context_compressor.cpp#L53) 的 `compress()`：

- `result.erase(result.begin(), result.begin() + drop)` —— 删头部，**前缀彻底失效**
- `summarize_tool_result` 原地改写旧 tool_result —— **中段前缀失效**
- 触发条件是固定 `max_messages=50` / `max_tokens_estimate=8000`，与真实窗口（DeepSeek 1M）完全脱节

### 缺口 3：DeepSeek 缓存字段未解析

- [openai_adapter.cpp:154](file:///d:/develop/Workspace/workx/src/agent/api/provider/openai_adapter.cpp#L154) 解析了 `prompt_tokens` / `completion_tokens`
- **未解析** `prompt_cache_hit_tokens` / `prompt_cache_miss_tokens`
- [chat_types.h:120](file:///d:/develop/Workspace/workx/src/agent/api/chat_types.h#L120) 的 `cache_creation_input_tokens` / `cache_read_input_tokens` 是 Anthropic 语义，对 DeepSeek 恒为 0
- **命中率完全不可见**

### 缺口 4：无前缀形状追踪、无诊断

没有任何机制检测"为什么这一轮缓存掉了"。system prompt 动态拼接（含 cwd/平台信息）若每轮变化，会静默击穿缓存且无法归因。

### 缺口 5：reasoning_content 不往返 ✅ 已修复（P2）

[openai_adapter.cpp](file:///d:/develop/Workspace/workx/src/agent/api/provider/openai_adapter.cpp) 原注释"reasoning_content 不发送给模型"。

- 对非 DeepSeek 后端是正确的（默认 false 保持兼容）
- 对 DeepSeek-reasoner 错失了 CoT 作为缓存前缀的机会

**P2 修复**：新增 `backend.send_reasoning_content` 配置（默认 false），开启后 `OpenAIAdapter` 把 assistant 消息的 `reasoning_content` 序列化为请求体字段往返发送。配置链路：`config.json` → `BackendConfig.send_reasoning_content` → `OpenAIAdapter::set_send_reasoning_content()`。A/B 测试时通过此开关切换即可测量净收益。

---

## 三、设计目标

### 3.1 核心目标

1. **可观测**：DeepSeek 缓存命中字段可见，会话级累计可显示，前缀抖动可归因
2. **可维持**：把 append-only 从"偶然正确"变为 enforced 不变量
3. **可恢复**：长对话不撑爆窗口的同时维持高命中，窗口太小时能自愈
4. **向后兼容**：不破坏现有 Anthropic 路径，DeepSeek 优化为增量能力

### 3.2 非目标

- ❌ 不替换 tokenizer 为精确实现（继续用 `chars/4` 启发式，与 Reasonix 一致）
- ❌ 不引入 Anthropic 显式 cache_control（DeepSeek 是隐式缓存，无需）
- ❌ 不改变 ReAct 主循环结构（仅在 `build_request` 前后插入钩子）
- ❌ 不改变 EventBus 异步事件流（仅新增诊断事件，不修改既有事件语义）

---

## 四、分层方案

按 ROI 排序，分 4 个层次。建议按 P0 → P1 → P2 顺序实施，**P0 落地后先用真实数据确认基线**再决定后续优先级。

### 层次 1：可观测性（P0）

**目标**：让缓存命中可见、可归因。这是后续一切优化的前提。

**改动点**：

#### 1.1 扩展 StreamChunk / StreamDoneEvent

[chat_types.h](file:///d:/develop/Workspace/workx/src/agent/api/chat_types.h) 的 `StreamChunk` 新增：

```cpp
// DeepSeek/OpenAI 兼容字段（与 Anthropic 的 cache_creation/cache_read 并存）
int32_t prompt_cache_hit_tokens = 0;    // DeepSeek usage.prompt_cache_hit_tokens
int32_t prompt_cache_miss_tokens = 0;   // DeepSeek usage.prompt_cache_miss_tokens
```

[stream_events.h](file:///d:/develop/Workspace/workx/src/core/events/stream_events.h) 的 `StreamDoneEvent` 同步新增上述两个字段。

#### 1.2 OpenAIAdapter 解析缓存字段

[openai_adapter.cpp](file:///d:/develop/Workspace/workx/src/agent/api/provider/openai_adapter.cpp) 有**两处** usage 解析（choices 空的 usage chunk 在 154 行，finish_reason 的 usage 在 220 行），都要加：

```cpp
if (usage.contains("prompt_cache_hit_tokens")) {
    out.prompt_cache_hit_tokens = usage["prompt_cache_hit_tokens"].get<int32_t>();
}
if (usage.contains("prompt_cache_miss_tokens")) {
    out.prompt_cache_miss_tokens = usage["prompt_cache_miss_tokens"].get<int32_t>();
}
```

#### 1.3 ReActLoop 透传

- `ThoughtResult`（[react_loop.h:255](file:///d:/develop/Workspace/workx/src/agent/core/react_loop.h#L255)）新增两字段
- `ReActResult` 同步新增
- `ChatSession::run_completion` 把它们写入 `StreamDoneEvent`

#### 1.4 会话级累计

会话级累计由 `token_stats_model`（TUI 侧）持有，直接累加 `StreamDoneEvent` 中的
`prompt_cache_hit_tokens` / `prompt_cache_miss_tokens`。

UI 状态栏显示 `cache 87% (session)` 而非单轮波动值（对齐 Reasonix `TestSessionAggregateCacheRate`）。

> **M-3 说明**：早期设计在 `ChatSession` 侧增加 `m_cache_hit_total` / `m_cache_miss_total` 与
> `session_cache_stats()`，但 TUI 实际使用 `token_stats_model` 自有累计器，导致双重累加器混淆。
> 已删除 `ChatSession` 侧的死代码，统一由 `token_stats_model` 累计。

#### 1.5 前缀形状诊断

新增 `agent/compact/prefix_shape.h`：

```cpp
struct PrefixShape {
    std::string system_hash;     // sha256(system_prompt) 前 8 字节的十六进制
    std::string tools_hash;      // sha256(tools_schema 规范化后) 前 8 字节的十六进制
    std::string prefix_hash;     // system + "|" + tools 联合指纹
    int rewrite_version = 0;     // 历史改写版本号
};

// capture_shape 在 ChatSession 中两次调用：
// 1. run() 前用 rewrite_version=0 捕获基线
// 2. run() 后用 react_result.rewrite_version 二次捕获，使 log_rewrite 归因生效（H-2）
PrefixShape capture_shape(const std::string& sys, const nlohmann::json& tools,
                          int rewrite_version = 0);

struct PrefixChangeDiagnosis {
    std::string prefix_hash;
    bool prefix_changed = false;
    std::vector<std::string> reasons;  // "system" / "tools" / "log_rewrite"
    int cache_hit_tokens = 0;
    int cache_miss_tokens = 0;
};

PrefixChangeDiagnosis compare_shape(const PrefixShape& prev,
                                     const PrefixShape& cur,
                                     int hit_tokens, int miss_tokens);
```

在 `ReActLoop::run` 每轮 `build_request` 前后捕获形状，通过 EventBus 发布 `CacheDiagnosticsEvent`。**只观测，不改行为**。

tools schema 规范化：按 `function.name` 排序后序列化（对齐 Reasonix `normalizeToolSchemas`）。
该逻辑已公开为 `normalize_tools_schema()`，同时用于：
- `capture_shape` 的 hash 计算
- `build_request` 的 tools 字段赋值（M-2：消除注册顺序抖动导致的缓存击穿）

**L-4 实现**：`system_hash` / `tools_hash` 使用手写 SHA-256（`agent/compact/sha256.{h,cpp}`，FIPS 180-4），
取前 8 字节（16 位十六进制）。跨编译器确定、跨进程稳定，可由 Python `hashlib.sha256(...).hexdigest()[:16]` 外部复算。

**H-2 透传链路**：
- `ReActResult` 新增 `int32_t rewrite_version`，`run()` 末尾回填 `m_compactor.rewrite_version()`
- `ChatSession::run_completion` 在 `run()` 返回后二次捕获 `cur_shape`，传入 `react_result.rewrite_version`
- 使 `compare_shape` 在 compact 改写历史中段后能正确触发 `log_rewrite` 归因

---

### 层次 2：前缀稳定性防护（P0）

**目标**：把 append-only 从"偶然正确"变为 enforced 不变量。

#### 2.1 Session 引入 RewriteVersion

`ChatSession` 增加 `m_rewrite_version` 计数器。任何对历史消息的非追加修改（未来的压缩）都递增它并发出诊断事件。前缀形状捕获它，**任何改写都能在命中率曲线上被看到**。

#### 2.2 system prompt 冻结契约

审计 `ChatSession::set_system_prompt` 调用时机。system prompt 应**仅在会话开始时构造一次**，之后字节不变。

若含动态内容（cwd / 时间戳 / 平台），必须集中在"可变尾段"并用稳定分隔符隔离：

```
[稳定段：身份 + 能力 + 工具指引]   ← 永不变，DeepSeek 公共前缀检测会落盘
---
[可变段：cwd / platform / 时间]   ← 变化时只丢这一段的缓存
```

#### 2.3 tools schema 规范化与冻结

工具表顺序若每轮不同（按注册序），序列化结果会抖动。

- `build_request` 中对 tools schema **按 name 排序**后序列化
- 计算 hash 纳入前缀形状
- 工具表在会话中应不变；动态启停工具会击穿缓存，需通过诊断事件暴露

#### 2.4 标记 ContextCompressor 为 deprecated

[context_compressor.h](file:///d:/develop/Workspace/workx/src/agent/compact/context_compressor.h) 的 `compress()` 当前实现是缓存杀手。在层次 3 重写前，**明确标记为 deprecated**，确保 `m_compressor` 不被误调用（目前是死代码，正好）。

---

### 层次 3：缓存感知压缩（P1）

**目标**：长对话不撑爆窗口的同时维持高命中。直接借鉴 Reasonix 分级方案，适配 C++。

#### 3.1 真实窗口驱动

`ReActLoop::Config` 增加：

```cpp
int32_t context_window_tokens = 0;  // 从 provider preset 读取，DeepSeek=1000000
```

压缩阈值改为窗口比例，而非固定 `max_messages=50`。

**H-4 注入路径**：`factory.cpp::create_session` 中读取 `cfg.CONTEXT_LENGTH` 或 `preset.default_context_length`，
通过 `ChatSession::set_compactor_context_window()` → `CacheAwareCompactor::set_context_window()` 注入。
优先级：`cfg.backend.context_length` > `preset.default_context_length` > 0（压缩器内部 fallback 1M）。

#### 3.2 分级触发

新增 `agent/compact/cache_aware_compactor.h`，替换 `ContextCompressor`：

```cpp
class CacheAwareCompactor {
public:
    // 水位比例（对齐 Reasonix）
    static constexpr float kSoftRatio   = 0.5f;  // 仅 Notice
    static constexpr float kSnipRatio   = 0.6f;  // 截短旧 tool_result
    static constexpr float kCompactRatio = 0.8f; // 摘要中段
    static constexpr float kForceRatio  = 0.9f;  // 强制折叠
    static constexpr int32_t kTailTokens = 16384; // 尾部预算

    void maybe_compact(const Usage& u);

private:
    // 1. soft: 发 CacheSoftNoticeEvent，return
    // 2. snip: snip_stale_tool_results()，return
    // 3. compact: pin_prefix + partition_fold + summarize + rewrite
    // 4. stuck guard: 连续 2 次 compact 仍超阈值 → 暂停自动压缩
};
```

#### 3.3 钉住前缀

`pinned_prefix_len()` 返回 `system + 第一条 user（若 < 1500 token）+ 已有摘要`。这些**永不折叠**。

预算自适应：`min(1500, context_window * 0.15)`，避免大首轮（粘贴内容）被钉住撑爆窗口。

#### 3.4 中段折叠

只对 `msgs[head:start]` 中非 pinned、非 keep_policy 的消息调用摘要 API：

- 摘要包在 `<compaction-summary>` 标签内作为 user 消息插入
- 原消息归档到 `sessions/<id>/archive/<timestamp>.jsonl` 保证可追溯
- 摘要失败时机械折叠（占位符 + 归档路径），不阻塞流程

**M-1 归档落地**：`compact_middle` 折叠前将中段消息序列化追加到 `<archive_dir>/<timestamp>.jsonl`。
`archive_dir` 由 `ChatSession::set_compactor_archive_dir()` 注入（factory 从 `save_path` 派生）。
摘要占位符中标注归档路径，保证原消息可追溯。

**M-4 LLM 摘要回调**：`CacheAwareCompactor::set_summarize_fn(SummarizeFn)` 注入 LLM 摘要。
`ChatSession::summarize_with_llm()` 同步调用 `m_provider->submit_completion` 生成结构化摘要：
- system prompt 指示压缩为结构化摘要（保留意图/操作/待办/关键信息）
- 低温度（0.3）+ max_tokens=1024 保证忠实
- 失败（null/Error/Cancelled/空）抛异常，由 `compact_middle` 的 try/catch 自动 fallback 到 `mechanical_fold_summary`
- 未注入 `summarize_fn` 时默认走机械折叠（P1 baseline）

#### 3.5 尾部预算

`tail_start()` 从尾部往前累积 token 直到预算用尽，对齐到非 tool 消息（避免孤儿 tool result）。

#### 3.6 卡死守卫

`m_consecutive_compacts >= 2` 时置 `m_compact_stuck = true`：
- 暂停自动压缩
- 发 `CompactionPausedEvent`
- 让前缀重新 append-only 增长

**H-3 自愈路径**：卡死后并非永久禁用。当 `ratio` 回落到 `soft_ratio` 以下时：
- 清除 `m_stuck`，重置 `m_consecutive_compacts`
- 发布 `CompactionPausedEvent{paused=false}` 恢复事件
- 让前缀重新 append-only 增长恢复缓存命中后，自动恢复压缩

`CompactionPausedEvent` 定义在 `core/events/agent_events.h`，由 `ChatSession` 通过
`CacheAwareCompactor::set_paused_callback()` 注册的回调发布到 EventBus。

**M-5 会话重置**：`ChatSession::clear_history()` 调用 `m_compactor.reset()`，
清除 `m_stuck` / `m_consecutive_compacts` / `m_rewrite_version`，避免跨会话泄漏。

**M-6 压缩点**：压缩在 `run()` 的 `iteration == 1` 执行（turn 间压缩），
而非 `iteration > 1`（turn 内每轮），避免单 turn 多步推理链被过早截短。

---

### 层次 4：tool_result 维护（P1，与层次 3 并行）

**目标**：在昂贵摘要之前，用机械方式释放空间。

新增 `agent/compact/tool_result_maintainer.h`：

```cpp
struct SnipStrategy {
    int head;
    int tail;
    int head_chars;
    int tail_chars;
};

// 只读工具：头长尾短（答案在前）  {80, 12, 10000, 2000}
// 副作用工具：头尾均等（错误可能在尾）{40, 40, 8000, 8000}
// 默认值对齐 Reasonix defaultReadOnlySnip / defaultSideEffectingSnip

struct SnipStats {
    int results = 0;
    int saved_chars = 0;
    std::string archive;
};

SnipStats snip_stale_tool_results(std::vector<ChatMessage>& msgs,
                                   int head_idx, int tail_start,
                                   const std::string& archive_dir);
SnipStats prune_stale_tool_results(std::vector<ChatMessage>& msgs,
                                    int head_idx, int tail_start,
                                    const std::string& archive_dir);
```

行为：
- **Snip**：保留头/尾若干行，中段替换为 `[snipped tool result — X archived to Y]`
- **Prune**：替换为 `[elided tool result — X bytes archived to Y]`
- **不删消息**，不改 tool_call_id，不改 assistant content
- 工具可实现 `SnipHint` 接口提供自定义策略（如 `GrepTool` 头短尾长）

**这层不调用 LLM**，零额外成本，在 snip 水位即可释放大量 token。

---

## 五、实施优先级

| 阶段 | 内容 | 价值 | 风险 | 预估工作量 |
|---|---|---|---|---|
| **P0** | 层次 1：解析 DeepSeek 缓存字段 + 会话累计 + UI 显示 | 让命中率可见，建立优化基线 | 极低，纯读取 | 1-2 天 |
| **P0** | 层次 2：system prompt 冻结 + tools 规范化 + 前缀诊断 | 防止最常见的前缀抖动 | 低，需审计 system 构造 | 2-3 天 |
| **P1** | 层次 3：CacheAwareCompactor 分级压缩 | 长对话不撑爆窗口且维持命中 | 中，需充分测试压缩正确性 | 1-2 周 |
| **P1** | 层次 4：tool_result snip | 廉价释放空间，推迟摘要调用 | 低，机械操作可逆 | 3-5 天 |
| **P2** ✅ | reasoning_content 可选往返 | DeepSeek-reasoner 的 CoT 进前缀 | 需 A/B 测量净收益 | 已实现 |

---

## 六、核心建议

**先做 P0**。当前 workx 的 `ContextCompressor` 是死代码，前缀天然 append-only，命中率本应不低——但**完全看不到它**。

层次 1 落地后，先用真实数据确认基线，再决定层次 3/4 的优先级：

- 如果 system prompt 含动态时间戳/cwd 且每轮变，**层次 2 的收益会比层次 3 还大**
- 如果长对话撑爆窗口是主要痛点，**层次 3 优先**
- 如果 tool_result 输出冗长（如 Read 大文件、Grep 海量匹配），**层次 4 优先**

---

## 七、验收标准

### 7.1 P0 验收

- [x] DeepSeek 请求的 `prompt_cache_hit_tokens` / `prompt_cache_miss_tokens` 被正确解析
- [x] UI 状态栏显示会话级累计命中率（非单轮波动值）
- [x] `CacheDiagnosticsEvent` 在 system/tools 变化时发布，UI 可见归因
- [x] system prompt 在会话中字节不变（含动态内容时集中在可变尾段）
- [x] tools schema 按 name 排序后序列化，hash 纳入前缀形状
- [x] 现有 Anthropic 路径不受影响（cache_creation/cache_read 仍工作）
- [x] **M-2**：`build_request` 中对 tools schema 按 `function.name` 排序后再赋值（消除注册顺序抖动导致的缓存击穿，与 `normalize_tools_schema` 复用同一逻辑）
- [x] **L-4**：前缀形状 hash 使用手写 SHA-256（FIPS 180-4），替代 `std::hash`；跨编译器确定、跨进程稳定、可外部验证（Python `hashlib.sha256` 复算）

### 7.2 P1 验收

- [x] 长对话（>50 轮）在 DeepSeek 1M 窗口下不撑爆
- [x] 压缩触发后下一轮命中率不崩塌（对齐 Reasonix `TestCacheHitSurvivesTooSmallWindow`：尾部 5 轮平均 ≥85%）
- [x] 卡死守卫在窗口太小时触发 `CompactionPausedEvent`，命中率恢复
- [x] tool_result snip 在 snip 水位释放 ≥50% 的旧 tool_result 字节
- [x] 压缩归档文件可读，原消息可追溯
- [x] **H-2**：`rewrite_version` 透传链路打通 — `ReActResult` 携带该字段，`run()` 末尾回填 `m_compactor.rewrite_version()`，`ChatSession` 二次捕获 `cur_shape` 时传入，使 `compare_shape` 的 `log_rewrite` 归因生效
- [x] **H-3**：卡死守卫新增 `CompactionPausedEvent` + 自愈路径 — ratio 回落到 `soft_ratio` 以下时清除 `m_stuck` 并发布恢复事件（paused=false），避免永久禁用
- [x] **H-4**：`compactor_cfg.context_window_tokens` 从 provider preset 注入 — `factory.cpp` 读取 `cfg.CONTEXT_LENGTH` 或 `preset.default_context_length`，通过 `set_compactor_context_window()` 注入
- [x] **M-1**：压缩归档落地 — `compact_middle` 折叠前将中段消息序列化追加到 `<archive_dir>/<timestamp>.jsonl`，摘要占位符中标注归档路径
- [x] **M-3**：删除 `ChatSession::session_cache_stats()` 死代码 — TUI 统一使用 `token_stats_model` 自有累计器，消除双重累加器混淆
- [x] **M-4**：LLM 摘要回调注入 — `ChatSession::summarize_with_llm()` 同步调用 `m_provider` 生成结构化摘要，失败自动 fallback 到 `mechanical_fold_summary`
- [x] **M-5**：`clear_history()` 重置压缩器状态 — 调用 `m_compactor.reset()` 清除 `m_stuck` / `m_consecutive_compacts` / `m_rewrite_version`，避免跨会话泄漏
- [x] **M-6**：压缩点移至 turn 间 — 从 `iteration > 1` 改为 `iteration == 1`，避免单 turn 内多步推理链被过早截短
- [x] **L-1**：移除 `cache_aware_compactor.cpp` 恒真冗余条件 `ratio < force_ratio || ratio >= compact_ratio`
- [x] **L-2**：`chat_renderer.cpp` 的 `EventToken` 订阅统一改用 `std::make_unique`
- [x] **H-1**：补齐 compact 模块单元测试 — 33 个测试用例覆盖 prefix_shape / tool_result_maintainer / cache_aware_compactor / sha256 四个模块（四档水位、pinned/tail 边界、卡死守卫触发与自愈、归档、工具分类、snip 幂等性、NIST FIPS 180-4 官方向量）

### 7.3 回归测试

参考 Reasonix 的 e2e 测试设计（`cachehit_e2e_test.go`），在 workx 中新增：

- `TestCacheHitPrefixStable`：断言 `hitChars[i] == reqChars[i-1]`
- `TestCacheHitClimbsWithoutCompaction`：14 轮对话峰值 ≥90%
- `TestCacheHitSurvivesTooSmallWindow`：小窗口下崩塌次数 ≤2，尾部平均 ≥85%
- `TestSessionAggregateCacheRate`：会话累计 = 单轮累加和

---

## 八、参考文件索引

### 8.1 Reasonix 参考实现

- [cache_shape.go](file:///d:/develop/Workspace/workx/example/DeepSeek-Reasonix/internal/agent/cache_shape.go) —— 前缀形状捕获与对比
- [compact.go](file:///d:/develop/Workspace/workx/example/DeepSeek-Reasonix/internal/agent/compact.go) —— 分级压缩主逻辑
- [prune.go](file:///d:/develop/Workspace/workx/example/DeepSeek-Reasonix/internal/agent/prune.go) —— tool_result 两级维护
- [run_loop.go](file:///d:/develop/Workspace/workx/example/DeepSeek-Reasonix/internal/agent/run_loop.go) —— 主循环中的缓存钩子
- [cachehit_e2e_test.go](file:///d:/develop/Workspace/workx/example/DeepSeek-Reasonix/internal/agent/cachehit_e2e_test.go) —— e2e 测试范式

### 8.2 workx 待改文件

- [src/agent/api/chat_types.h](file:///d:/develop/Workspace/workx/src/agent/api/chat_types.h) —— StreamChunk 扩展
- [src/core/events/stream_events.h](file:///d:/develop/Workspace/workx/src/core/events/stream_events.h) —— StreamDoneEvent 扩展
- [src/agent/api/provider/openai_adapter.cpp](file:///d:/develop/Workspace/workx/src/agent/api/provider/openai_adapter.cpp) —— 缓存字段解析
- [src/agent/core/react_loop.h](file:///d:/develop/Workspace/workx/src/agent/core/react_loop.h) / [.cpp](file:///d:/develop/Workspace/workx/src/agent/core/react_loop.cpp) —— 透传与钩子
- [src/agent/core/chat_session.h](file:///d:/develop/Workspace/workx/src/agent/core/chat_session.h) / [.cpp](file:///d:/develop/Workspace/workx/src/agent/core/chat_session.cpp) —— 会话级累计
- [src/agent/compact/context_compressor.h](file:///d:/develop/Workspace/workx/src/agent/compact/context_compressor.h) / [.cpp](file:///d:/develop/Workspace/workx/src/agent/compact/context_compressor.cpp) —— 标记 deprecated，后续替换
- [src/agent/model/provider_preset.cpp](file:///d:/develop/Workspace/workx/src/agent/model/provider_preset.cpp) —— context_window 配置源
