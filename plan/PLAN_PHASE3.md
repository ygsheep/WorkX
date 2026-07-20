# WorkX 修复 Plan — 阶段 3：API 适配正确性 P1

> **状态**：待审批（未做任何代码修改）
> **范围**：仅修复 `agent/api` 模块的 P1 问题 + 1 项阶段 1 遗漏的 P0
> **依据**：[CODE_REVIEW_REPORT.md](file:///c:\Users\young\Desktop\Develop\WorkX\CODE_REVIEW_REPORT.md) 第 4 节
> **关联阶段**：依赖阶段 1（Client/EventBus/TaskManager 并发修复）已完成

---

## 0. 阶段 3 修复范围

| 编号 | 级别 | 问题 | 文件 |
|------|------|------|------|
| 4.6 | P0（阶段 1 遗漏） | HTTP 总超时被错误用于流式传输 | `http_client.cpp` |
| 4.9 | P1 | `chat(user_text)` 失败时历史已污染 | `client.cpp` |
| 4.10 | P1 | `cancel_stream(const SSEStreamReader*) const` 声明未实现 | `http_client.{h,cpp}` |
| 4.11 | P1 | OpenAI 流式 usage 永远为 0 | `openai_adapter.cpp` |
| 4.12 | P1 | Anthropic `list_models` 端点错误 | `remote_backend.cpp` |
| 4.13 | P1 | OpenAI `parse_sse_event` 仅处理第一个 tool_call | `openai_adapter.cpp` |
| 4.14 | P1 | OpenAI finish_reason 缺 `content_filter` | `openai_adapter.cpp` |
| 4.15 | P1 | Anthropic 用 `operator[]` 访问可能不存在的键 | `anthropic_adapter.cpp` |
| 4.16 | P1 | Anthropic assistant reasoning_content 被拼进 content | `anthropic_adapter.cpp` |
| 4.17 | P1 | Anthropic 多个 tool_result 被拆成多条 user 消息 | `anthropic_adapter.cpp` |
| 4.18 | P1 | OpenAI assistant with tool_calls 时 content 字段处理不当 | `openai_adapter.cpp` |
| 4.19 | P1 | OpenAI Tool 消息缺少 tool_call_id 校验 | `openai_adapter.cpp` |
| 4.20 | P1 | Anthropic system_prompt 多条只取最后一条 | `anthropic_adapter.cpp` |
| 4.21 | P1 | Anthropic `message_delta` 不解析 `input_tokens` | `anthropic_adapter.cpp` |
| 4.22 | P1 | `async_post_stream` 提交失败时状态不一致 | `http_client.cpp` |
| 4.23 | P1 | HTTP 错误响应体被丢弃 | `http_client.cpp` |
| 4.24 | P1 | OpenAI 流式 error 事件被静默吞掉 | `openai_adapter.cpp` |
| 4.25 | P1 | `RemoteBackend::m_retry_count`/`m_retry_delay_ms` 死代码 | `remote_backend.{h,cpp}` |
| 4.26 | P1 | `RemoteBackend::submit_completion` 不检查是否已有在飞请求 | `remote_backend.cpp` |
| 4.27 | P1 | `Client::regenerate` 不检查 generating | `client.cpp` |
| 4.28 | P1 | 重试退避 `1 << attempt` 溢出风险 | `client.cpp` |
| **合计** | **1 P0 + 20 P1** | **21 项** | **6 个文件** |

> 其他模块（agent/core 第 2 章、agent/command 第 5 章、core/app 第 8 章）的 P1 留待阶段 4

---

## 1. HTTP 流式传输超时修复（1 项 P0，阶段 1 遗漏）

### 1.1 问题清单
- **4.6** [http_client.cpp:162](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\remote\http_client.cpp#L162) `CURLOPT_TIMEOUT_MS` 是整个传输总时长，30000ms 超时会让长响应被中途切断

### 1.2 修复方案

**思路**：流式传输不能用总时长超时（长响应正常），应改用空闲超时（无数据传输 N 秒则断开）。

修改 [http_client.cpp:162](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\remote\http_client.cpp#L162) `StreamSession` 构造函数：

```cpp
// 修改前：
// curl_easy_setopt(m_curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms));

// 修改后：流式传输使用空闲超时，不用总超时
// CURLOPT_LOW_SPEED_LIMIT + CURLOPT_LOW_SPEED_TIME:
//   若传输速度低于 LOW_SPEED_LIMIT bytes/s 持续 LOW_SPEED_TIME 秒，则断开
// 设置 LOW_SPEED_LIMIT=1 byte/s, LOW_SPEED_TIME=timeout_ms/1000 秒
// 即：在 timeout_ms 时间内无任何数据传输则断开
const long idle_seconds = std::max(1L, static_cast<long>(timeout_ms / 1000));
curl_easy_setopt(m_curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
curl_easy_setopt(m_curl, CURLOPT_LOW_SPEED_TIME, idle_seconds);
// 保留连接超时（10 秒）
curl_easy_setopt(m_curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
```

**注**：同步 GET（`HttpClient::get`）仍保留 `CURLOPT_TIMEOUT_MS`（[http_client.cpp:104](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\remote\http_client.cpp#L104)），因为 list_models 是非流式短请求。

### 1.3 风险与缓解
- **长空闲连接被断开**：若 LLM 思考时间长（如 reasoning model 60s+），idle_seconds=30s 会误断。**缓解**：把 timeout_ms 配置默认值从 30s 改为 120s，给 LLM 思考留足时间
- **测试**：测试「正常 30s 内响应」「长响应 60s 持续传输」「断网 30s 触发超时」

---

## 2. `chat(user_text)` 失败时历史污染修复（1 项 P1）

### 2.1 问题清单
- **4.9** [client.cpp:344-358](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\client.cpp#L344) `chat(user_text)` 在 `run_stream` **之前**就 `push_back` user 消息；失败时孤儿消息留在历史
- [client.cpp:374-388](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\client.cpp#L374) `stream_chat(user_text)` 同样问题
- 而 `chat(vector<ChatMessage>&)` 和 `stream_chat(vector<ChatMessage>&)` 又不 push，行为不一致

### 2.2 修复方案

**策略**：失败时回滚（pop 掉刚 push 的 user 消息）。

```cpp
Result<std::string, std::string> Client::chat(const std::string& user_text) {
    auto request = build_request(user_text);
    {
        std::lock_guard<std::mutex> lock(m_messages_mutex);  // 阶段 1 已加
        m_messages.push_back(ChatMessage::user(user_text));
    }

    m_generating.store(true);
    std::string content, reasoning;
    auto result = run_stream(request, {}, []() { return false; }, content, reasoning);
    m_generating.store(false);

    if (result.isOk()) {
        {
            std::lock_guard<std::mutex> lock(m_messages_mutex);
            m_messages.push_back(ChatMessage::assistant(content));
        }
        return Result<std::string, std::string>::ok(std::move(content));
    }
    // 失败时回滚 user 消息
    {
        std::lock_guard<std::mutex> lock(m_messages_mutex);
        if (!m_messages.empty() && m_messages.back().role == ChatMessage::Role::User) {
            m_messages.pop_back();
        }
    }
    return Result<std::string, std::string>::err(result.error());
}
```

同样修改 `stream_chat(user_text, cbs)`。

### 2.3 风险与缓解
- **用户重试时丢失上次输入**：失败回滚后用户看不到自己上次的输入。**缓解**：TUI 层可单独保留输入文本用于显示；Client 的职责是维护 LLM 上下文，不该有孤儿消息
- **与 `chat(vector<ChatMessage>&)` 行为不一致**：后者由调用方管理历史，不自动 push。**保持现状**，仅文档说明差异

---

## 3. `cancel_stream(const SSEStreamReader*) const` 实现补全（1 项 P1）

### 3.1 问题清单
- **4.10** [http_client.h:50](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\remote\http_client.h#L50) 声明 `cancel_stream(const SSEStreamReader*) const`，cpp 中无实现，调用即链接错误

### 3.2 修复方案

**选项 A（推荐）**：删除 const 重载（[http_client.h:50](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\remote\http_client.h#L50)），保留 [http_client.cpp:331](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\remote\http_client.cpp#L331) 已实现的非 const 版本。

```cpp
// http_client.h 中删除：
// void cancel_stream(SSEStreamReader* reader) const;  // 删除此行
```

**选项 B**：补充 const 实现（但 `m_impl` 是 `unique_ptr`，const 方法无法修改内部状态，需 `mutable`）。不推荐。

### 3.3 风险与缓解
- **若有调用方使用 const 版本**：编译报错。**缓解**：Grep 搜索所有 `cancel_stream` 调用，确认无 const 版本调用

---

## 4. OpenAI 流式 usage 修复（1 项 P1）

### 4.1 问题清单
- **4.11** [openai_adapter.cpp:182-187](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\provider\openai_adapter.cpp#L182) OpenAI 流式默认不返回 usage，必须设 `stream_options: {"include_usage": true}`
- 全工程未出现 `stream_options`/`include_usage`

### 4.2 修复方案

#### 4.2.1 build_request_body 添加 stream_options（[openai_adapter.cpp:42-44](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\provider\openai_adapter.cpp#L42)）

```cpp
j["model"] = model_name;
j["stream"] = request.stream;

// 新增：流式传输时启用 usage 上报
if (request.stream) {
    j["stream_options"] = {{"include_usage", true}};
}
```

#### 4.2.2 parse_sse_event 处理空 choices + usage（[openai_adapter.cpp:134-136](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\provider\openai_adapter.cpp#L134)）

```cpp
// 修改前：
// if (!json_obj.contains("choices") || json_obj["choices"].empty()) {
//     return false;
// }

// 修改后：choices 空但 usage 存在时（开启 include_usage 后最后 chunk）
if (!json_obj.contains("choices") || json_obj["choices"].empty()) {
    // 检查是否为 usage chunk（include_usage=true 时最后 chunk 的 choices 为空）
    if (json_obj.contains("usage") && !json_obj["usage"].is_null()) {
        const auto& usage = json_obj["usage"];
        out.prompt_tokens = usage.value("prompt_tokens", 0);
        out.generated_tokens = usage.value("completion_tokens", 0);
        // usage chunk 通常伴随 finish_reason，但若单独到达也标记为 final
        return true;
    }
    return false;
}
```

### 4.3 风险与缓解
- **某些 OpenAI 兼容 API 不支持 stream_options**：返回 400。**缓解**：DeepSeek、Together AI 等主流兼容 API 均支持；若失败可在配置中关闭（暂不做配置开关，问题出现后再加）
- **测试**：测试「正常流式 + 显示 token 数」「DeepSeek/Together AI 兼容性」

---

## 5. Anthropic `list_models` 端点修复（1 项 P1）

### 5.1 问题清单
- **4.12** [remote_backend.cpp:162-168](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\remote\remote_backend.cpp#L162) 无论 provider 都硬编码 `/v1/models`，Anthropic 无此端点 → 必然 404

### 5.2 修复方案

**策略**：Anthropic 没有公开的 list models 端点。返回内置模型列表。

#### 5.2.1 在 IProviderAdapter 增加 list_models 默认实现

```cpp
// i_provider_adapter.h 新增
struct ModelEndpointResult {
    bool supported;          // false 表示该 provider 不支持 list_models
    std::string url_suffix;  // 如 "/v1/models"
};

virtual ModelEndpointResult get_models_endpoint() const {
    return {false, ""};  // 默认不支持
}
```

#### 5.2.2 OpenAIAdapter 覆盖

```cpp
ModelEndpointResult OpenAIAdapter::get_models_endpoint() const {
    return {true, "/v1/models"};
}
```

#### 5.2.3 AnthropicAdapter 不覆盖（用默认 `{false, ""}`），或返回内置列表

```cpp
// anthropic_adapter.h
class AnthropicAdapter : public IProviderAdapter {
public:
    // ...
    std::vector<ModelInfo> get_builtin_models() const override;
};

// anthropic_adapter.cpp
std::vector<ModelInfo> AnthropicAdapter::get_builtin_models() const {
    return {
        {.name = "claude-sonnet-4-5-20250929", .description = "Anthropic Claude Sonnet 4.5", .context_length = 200000},
        {.name = "claude-opus-4-1-20250805", .description = "Anthropic Claude Opus 4.1", .context_length = 200000},
        {.name = "claude-haiku-4-5-20251001", .description = "Anthropic Claude Haiku 4.5", .context_length = 200000},
    };
}
```

#### 5.2.4 RemoteBackend::list_models 分发（[remote_backend.cpp:156-229](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\remote\remote_backend.cpp#L156)）

```cpp
Result<std::vector<ModelInfo>, std::string> RemoteBackend::list_models() {
    // 新增：检查 provider 是否支持 list_models 端点
    auto endpoint = m_adapter->get_models_endpoint();
    if (!endpoint.supported) {
        // 返回内置模型列表
        auto builtin = m_adapter->get_builtin_models();
        if (!builtin.empty()) {
            return Result<std::vector<ModelInfo>, std::string>::ok(std::move(builtin));
        }
        return Result<std::vector<ModelInfo>, std::string>::err(
            "This provider does not support list_models endpoint");
    }

    // 后续走原 HTTP GET 逻辑，URL 改用 endpoint.url_suffix
    std::string url = m_config.base_url;
    while (!url.empty() && url.back() == '/') url.pop_back();
    url += endpoint.url_suffix;
    // ... 原 HTTP GET 逻辑
}
```

### 5.3 风险与缓解
- **内置模型列表过时**：Anthropic 发布新模型时需手动更新。**缓解**：写明更新日期；用户可手动配置 model_name 跳过 list_models
- **接口变更**：IProviderAdapter 新增虚函数，所有子类需实现。**缓解**：用默认实现 `{false, ""}`，仅 OpenAI 覆盖

---

## 6. OpenAI tool_calls 多个增量修复（1 项 P1）

### 6.1 问题清单
- **4.13** [openai_adapter.cpp:152-175](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\provider\openai_adapter.cpp#L152) `delta["tool_calls"][0]` 只取第一个，丢弃 index > 0 的所有 tool_call

### 6.2 修复方案

**当前 StreamChunk 结构限制**：`out.tool_use_id`/`tool_name`/`tool_input_delta` 是单值，无法一次表达多个 tool_call 增量。

**两种方案**：

#### 方案 A（保守，推荐）：依赖 OpenAI 按 index 顺序发送

OpenAI 实际行为：多个 tool_call 时，每个 tool_call 的增量按 index 顺序分开发送（不会在同个 chunk 内混合多个 index 的增量）。所以只需处理 `delta["tool_calls"]` 数组中**第一个非空**元素即可，但要用 `index` 字段而非 `[0]`：

```cpp
// 修改前：
// const auto& tc = delta["tool_calls"][0];

// 修改后：处理数组中所有元素，但 StreamChunk 只能存一个，按 index 选第一个
if (delta.contains("tool_calls") && !delta["tool_calls"].empty()) {
    for (const auto& tc : delta["tool_calls"]) {
        const auto& func = tc.value("function", nlohmann::json::object());

        // 首次出现：带 id
        if (tc.contains("id") && !tc["id"].is_null()) {
            out.is_tool_use_start = true;
            out.tool_use_id = tc["id"].get<std::string>();
            out.tool_name = func.value("name", "");
            if (func.contains("arguments") && !func["arguments"].is_null()) {
                out.is_tool_use_delta = true;
                out.tool_input_delta = func["arguments"].get<std::string>();
            }
            return true;  // 一次只处理一个 tool_call start
        }

        // 后续：arguments 增量
        if (func.contains("arguments") && !func["arguments"].is_null()) {
            out.is_tool_use_delta = true;
            out.tool_input_delta = func["arguments"].get<std::string>();
            return true;  // 一次只处理一个 delta
        }
    }
}
```

**问题**：若 OpenAI 在同个 chunk 内发送多个 tool_call 的增量，只处理第一个。实际罕见，但需记录。

#### 方案 B（彻底）：StreamChunk 改为 vector

将 `out.tool_use_id/tool_name/tool_input_delta` 改为 `vector<ToolUseDelta>`，每次 SSE event 可携带多个。

**本次不采用**：改动面大，影响 ChatSession/ChatRenderer 等多处。留待后续重构。

### 6.3 风险与缓解
- **多 tool_call 同 chunk 增量丢失**：方案 A 限制。**缓解**：OpenAI 实际行为按 index 分开发送，影响可忽略；若发现问题再升级到方案 B

---

## 7. OpenAI finish_reason 补全 `content_filter`（1 项 P1）

### 7.1 问题清单
- **4.14** [openai_adapter.cpp:180](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\provider\openai_adapter.cpp#L180) 缺 `content_filter` 处理 → 流会卡住直到超时

### 7.2 修复方案

```cpp
// 修改前：
// if (finish_reason == "stop" || finish_reason == "length" || finish_reason == "tool_calls") {

// 修改后：新增 content_filter
if (finish_reason == "stop" || finish_reason == "length" ||
    finish_reason == "tool_calls" || finish_reason == "content_filter") {
    out.is_final = true;
    // content_filter 时可附加提示信息
    if (finish_reason == "content_filter") {
        out.content_delta = "[Content filtered by provider]";
    }
    // usage 信息（原逻辑）
    if (json_obj.contains("usage") && !json_obj["usage"].is_null()) {
        // ...
    }
}
```

### 7.3 风险与缓解
- **无风险**：纯补全逻辑

---

## 8. Anthropic `operator[]` 改 `.value()` 修复（1 项 P1）

### 8.1 问题清单
- **4.15** [anthropic_adapter.cpp:152, 166, 182](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\provider\anthropic_adapter.cpp#L152) 用 `operator[]` 访问可能不存在的键，缺字段时抛 `out_of_range`

### 8.2 修复方案

```cpp
// 修改前（line 152）：
// const auto& delta = json_obj["delta"];

// 修改后：
const auto& delta = json_obj.value("delta", nlohmann::json::object());

// 修改前（line 166）：
// const auto& block = json_obj["content_block"];

// 修改后：
const auto& block = json_obj.value("content_block", nlohmann::json::object());

// 修改前（line 182）：
// const auto& delta = json_obj["delta"];

// 修改后：
const auto& delta = json_obj.value("delta", nlohmann::json::object());
```

### 8.3 风险与缓解
- **无风险**：纯防御性访问

---

## 9. Anthropic reasoning_content 结构化输出修复（1 项 P1）

### 9.1 问题清单
- **4.16** [anthropic_adapter.cpp:89-93](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\provider\anthropic_adapter.cpp#L89) assistant 的 reasoning_content 被拼进 content：
  ```cpp
  m["content"] = msg.reasoning_content + "\n" + msg.content;
  ```
- 模型无法区分思考与回复，多轮对话严重劣化

### 9.2 修复方案

Anthropic 支持 `thinking` content block（需 API 版本 2025-01-01+）。但当前 `anthropic-version: 2023-06-01`（[anthropic_adapter.cpp:38](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\provider\anthropic_adapter.cpp#L38)）较旧。

**保守方案**：用多个 content block 区分 text 与 thinking（即使旧 API 也支持 content array）：

```cpp
case ChatMessage::Role::Assistant: {
    m["role"] = "assistant";
    if (!msg.tool_uses.empty()) {
        // 原工具调用逻辑
        // ...
    } else {
        // 新增：用 content blocks 数组结构化输出
        nlohmann::json content_blocks = nlohmann::json::array();

        // thinking block（若有 reasoning_content）
        if (!msg.reasoning_content.empty()) {
            content_blocks.push_back({
                {"type", "thinking"},
                {"thinking", msg.reasoning_content}
            });
        }

        // text block
        if (!msg.content.empty()) {
            content_blocks.push_back({
                {"type", "text"},
                {"text", msg.content}
            });
        }

        m["content"] = std::move(content_blocks);
    }
    messages.push_back(m);
    break;
}
```

**注**：`thinking` block 需要 `anthropic-version: 2025-01-01` 或更新。**配套修改**：[anthropic_adapter.cpp:38](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\provider\anthropic_adapter.cpp#L38) 升级版本号：

```cpp
headers.emplace_back("anthropic-version", "2023-06-01");  // 旧
// 改为
headers.emplace_back("anthropic-version", "2025-01-01");  // 新（支持 thinking block）
```

### 9.3 风险与缓解
- **API 版本升级影响**：2025-01-01 版本下其他字段格式可能变化。**缓解**：Anthropic Messages API 主要字段稳定；执行阶段需用真实 API Key 验证
- **旧版 API 用户**：若用户配置了旧 base_url 指向旧版 API，可能不兼容。**缓解**：Anthropic 已废弃旧版本，新版本向后兼容

---

## 10. Anthropic 多 tool_result 合并修复（1 项 P1）

### 10.1 问题清单
- **4.17** [anthropic_adapter.cpp:99-110](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\provider\anthropic_adapter.cpp#L99) 每个 Tool 消息生成单独的 user 消息：
  ```cpp
  m["role"] = "user";
  m["content"] = json::array({{{"type","tool_result"}, ...}});
  messages.push_back(m);
  ```
- Anthropic 规范要求连续 tool_result 必须放同一 user 消息的 content 数组

### 10.2 修复方案

**策略**：遍历消息时累积连续 Tool 消息，合并到一个 user 消息中。

```cpp
// 修改 build_request_body 中的消息遍历逻辑
nlohmann::json pending_tool_results = nlohmann::json::array();  // 累积连续 tool_result

for (const auto& msg : request.messages) {
    // 遇到非 Tool 消息时，先 flush 累积的 tool_results
    auto flush_tool_results = [&]() {
        if (!pending_tool_results.empty()) {
            nlohmann::json m;
            m["role"] = "user";
            m["content"] = std::move(pending_tool_results);
            messages.push_back(std::move(m));
            pending_tool_results = nlohmann::json::array();
        }
    };

    if (msg.role == ChatMessage::Role::System) {
        flush_tool_results();
        system_prompt = msg.content;  // 注：多条 system 仍取最后，4.20 单独修复
        continue;
    }

    if (msg.role == ChatMessage::Role::Tool) {
        // 累积 tool_result，不立即 push
        pending_tool_results.push_back({
            {"type", "tool_result"},
            {"tool_use_id", msg.tool_call_id},
            {"content", msg.content}
        });
        continue;
    }

    // User / Assistant：先 flush 累积的 tool_results
    flush_tool_results();

    nlohmann::json m;
    switch (msg.role) {
        case ChatMessage::Role::User:
            m["role"] = "user";
            m["content"] = msg.content;
            messages.push_back(std::move(m));
            break;
        case ChatMessage::Role::Assistant:
            // ... 原逻辑（含 9 节的 thinking block 改造）
            break;
        default:
            continue;
    }
}

// 循环结束后 flush 残留的 tool_results
if (!pending_tool_results.empty()) {
    nlohmann::json m;
    m["role"] = "user";
    m["content"] = std::move(pending_tool_results);
    messages.push_back(std::move(m));
}
```

### 10.3 风险与缓解
- **无风险**：符合 Anthropic API 规范，避免 400 错误

---

## 11. OpenAI assistant tool_calls content 字段修复（1 项 P1）

### 11.1 问题清单
- **4.18** [openai_adapter.cpp:54](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\provider\openai_adapter.cpp#L54) 强制 `m["content"] = ""`，应为 `null`（部分兼容 API 报错）

### 11.2 修复方案

```cpp
// 修改前：
// m["content"] = msg.content;

// 修改后：assistant with tool_calls 时 content 应为 null
if (msg.role == ChatMessage::Role::Assistant && !msg.tool_uses.empty()) {
    m["content"] = nullptr;  // OpenAI 规范要求 null
} else {
    m["content"] = msg.content;
}
```

**注**：原代码 line 54 无条件设 `m["content"] = msg.content`，line 62-75 才追加 `tool_calls`。改造后需在 tool_uses 检查内设置 content=null。

### 11.3 风险与缓解
- **空 content 字符串被某些 API 接受**：但官方规范是 null。**缓解**：改 null 更合规

---

## 12. OpenAI Tool 消息 tool_call_id 校验修复（1 项 P1）

### 12.1 问题清单
- **4.19** [openai_adapter.cpp:58-60](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\provider\openai_adapter.cpp#L58) 空时仍写入空字符串 → OpenAI 返回 400

### 12.2 修复方案

```cpp
if (msg.role == ChatMessage::Role::Tool) {
    if (msg.tool_call_id.empty()) {
        // 跳过无效 Tool 消息（或抛异常由上层处理）
        // 选择跳过：避免单条坏消息导致整个请求失败
        continue;
    }
    m["tool_call_id"] = msg.tool_call_id;
}
```

**配套**：在 Client 层增加 Tool 消息生成时的 tool_call_id 必填校验，从源头避免空值。

### 12.3 风险与缓解
- **静默丢弃 Tool 消息**：LLM 收不到工具结果，可能困惑。**缓解**：日志记录被跳过的消息；执行阶段在 ChatSession 层补 tool_call_id 必填检查

---

## 13. Anthropic 多 system_prompt 拼接修复（1 项 P1）

### 13.1 问题清单
- **4.20** [anthropic_adapter.cpp:56-60](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\provider\anthropic_adapter.cpp#L56) 多条 system 时后者覆盖前者

### 13.2 修复方案

```cpp
// 修改前：
// for (const auto& msg : request.messages) {
//     if (msg.role == ChatMessage::Role::System) {
//         system_prompt = msg.content;  // 取最后一条
//         continue;
//     }
//     ...

// 修改后：拼接多条 system_prompt
std::string system_prompt;
for (const auto& msg : request.messages) {
    if (msg.role == ChatMessage::Role::System) {
        if (!system_prompt.empty()) {
            system_prompt += "\n\n";  // 多条用空行分隔
        }
        system_prompt += msg.content;
        continue;
    }
    // ... 其他角色处理
}
```

**Anthropic 也支持 system 作为 content blocks 数组**（多条 system 用不同 type）。但保守方案用拼接。

### 13.3 风险与缓解
- **无风险**：拼接行为符合多数 LLM 期望

---

## 14. Anthropic `message_delta` 解析 input_tokens 修复（1 项 P1）

### 14.1 问题清单
- **4.21** [anthropic_adapter.cpp:189-192](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\provider\anthropic_adapter.cpp#L189) 只取 `output_tokens`，`prompt_tokens` 永远是 0

### 14.2 修复方案

Anthropic `message_delta` 事件的 `usage` 包含 `input_tokens`（不是 `prompt_tokens`）。

```cpp
// 修改前：
// if (json_obj.contains("usage") && !json_obj["usage"].is_null()) {
//     const auto& usage = json_obj["usage"];
//     out.generated_tokens = usage.value("output_tokens", 0);
// }

// 修改后：
if (json_obj.contains("usage") && !json_obj["usage"].is_null()) {
    const auto& usage = json_obj.value("usage", nlohmann::json::object());
    out.generated_tokens = usage.value("output_tokens", 0);
    // 新增：input_tokens（Anthropic 用 input_tokens 而非 prompt_tokens）
    out.prompt_tokens = usage.value("input_tokens", 0);
}
```

**注**：`message_start` 事件也携带 `usage.input_tokens`（初始值），但当前代码未处理 `message_start`。可在 `message_start` 分支补充：

```cpp
else if (event_type == "message_start") {
    // 新增：message_start 携带 input_tokens 初始值
    if (json_obj.contains("message") && json_obj["message"].contains("usage")) {
        const auto& usage = json_obj["message"].value("usage", nlohmann::json::object());
        out.prompt_tokens = usage.value("input_tokens", 0);
    }
    return false;  // 不算有效 chunk，仅记录
}
```

### 14.3 风险与缓解
- **无风险**：补充字段读取

---

## 15. `async_post_stream` 提交失败状态一致性修复（1 项 P1）

### 15.1 问题清单
- **4.22** [http_client.cpp:320](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\remote\http_client.cpp#L320) curl 初始化失败时直接 return，但调用方已把 reader 存入 `m_active_reader`
- reader 永远不会收到数据，`next()` 无限阻塞

### 15.2 修复方案

修改 [http_client.cpp:307-329](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\remote\http_client.cpp#L307) `async_post_stream`：

```cpp
void HttpClient::async_post_stream(...) const {
    auto parsed = parse_url(url);
    auto* key = reader.get();

    const auto session = std::make_shared<StreamSession>(
        parsed, headers, body, reader, std::move(on_complete), timeout_ms);

    if (!session->easy_handle()) {
        // 新增：curl 初始化失败时，主动 finish reader 让上层能收到错误
        reader->finish("Failed to initialize curl session");
        if (on_complete) on_complete();
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_impl->sessions_mutex);
        m_impl->sessions_by_handle[session->easy_handle()] = session;
        m_impl->sessions_by_reader[key] = session;
    }

    session->run(m_impl->multi);
}
```

### 15.3 风险与缓解
- **无风险**：补充错误路径，避免上层无限阻塞

---

## 16. HTTP 错误响应体保留修复（1 项 P1）

### 16.1 问题清单
- **4.23** [http_client.cpp:198-200](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\remote\http_client.cpp#L198) 只传状态码字符串，丢弃 JSON 错误体
- 上层只看到 "HTTP error: 429" 看不到 "rate_limit_exceeded, retry after 30s"

### 16.2 修复方案

修改 `StreamSession::on_transfer_done`（[http_client.cpp:191-205](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\remote\http_client.cpp#L191)）：

```cpp
void on_transfer_done(const CURLcode code) {
    if (m_cancelled.load()) { finish(""); return; }
    if (code != CURLE_OK) { finish(curl_easy_strerror(code)); return; }

    long http_code = 0;
    curl_easy_getinfo(m_curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code >= 400) {
        // 新增：把已接收的响应体作为错误信息一部分
        // StreamSession 当前未累积 body，需新增 m_error_body 成员
        std::string err_msg = std::string("HTTP error: ") + std::to_string(http_code);
        // 若有 body 内容，附上（截断防止过长）
        if (!m_error_body.empty()) {
            err_msg += " - " + m_error_body.substr(0, 500);
        }
        finish(err_msg);
        return;
    }
    // ... 正常完成逻辑
}
```

**配套**：在 `stream_write_cb` 中累积 4xx/5xx 响应体到 `m_error_body`。但当前 `stream_write_cb` 直接把数据喂给 reader。**简化方案**：先检查 HTTP 状态码，4xx/5xx 时不喂给 reader，而是累积到 `m_error_body`。

```cpp
static size_t stream_write_cb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* self = static_cast<StreamSession*>(userdata);
    if (self->m_cancelled.load()) return 0;

    // 新增：HTTP 错误响应时累积 body 作为错误信息
    long http_code = 0;
    curl_easy_getinfo(self->m_curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (http_code >= 400) {
        self->m_error_body.append(static_cast<const char*>(ptr), size * nmemb);
        return size * nmemb;
    }

    if (self->m_reader)
        self->m_reader->feed_data(std::string(static_cast<const char*>(ptr), size * nmemb));
    return size * nmemb;
}
```

**注**：同步 GET 的 `get()` 方法已正确保留 body（[http_client.cpp:115-127](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\remote\http_client.cpp#L115)），但 [remote_backend.cpp:189-192](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\remote\remote_backend.cpp#L189) `list_models` 已正确传 body。本节仅修流式路径。

### 16.3 风险与缓解
- **m_error_body 无限增长**：恶意服务器返回大 body 占内存。**缓解**：限制累积长度（如 8KB）
- **每 chunk 调用 curl_easy_getinfo 开销**：性能损失。**缓解**：在 `on_transfer_done` 一次性检查，stream_write_cb 用 flag 标记

---

## 17. OpenAI 流式 error 事件处理修复（1 项 P1）

### 17.1 问题清单
- **4.24** [openai_adapter.cpp:129-131](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\provider\openai_adapter.cpp#L129) `return false` 不设 is_final、不填错误信息，流继续等待最终超时

### 17.2 修复方案

**当前 StreamChunk 结构无 error_message 字段**，需新增（或在 content_delta 中塞错误提示）：

```cpp
// 修改前：
// if (json_obj.contains("error")) {
//     return false;
// }

// 修改后：
if (json_obj.contains("error")) {
    const auto& err = json_obj["error"];
    std::string err_msg = err.value("message", "Unknown OpenAI error");
    // 把错误信息塞入 content_delta（让用户看到）+ 标记 final
    out.content_delta = std::string("[OpenAI Error] ") + err_msg;
    out.is_final = true;
    return true;
}
```

**配套**：在 StreamChunk 中新增 `std::string error_message` 字段（更优雅）：

```cpp
struct StreamChunk {
    // ... 原字段
    std::string error_message;  // 新增
    bool has_error = false;     // 新增
};
```

但新增字段需改 StreamChunk 定义（[chat_types.h](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\chat_types.h)），影响面大。**本次采用塞入 content_delta 方案**，新增字段留待后续。

### 17.3 风险与缓解
- **错误信息混入正常 content**：用户看到 `[OpenAI Error]` 前缀可识别。**缓解**：前缀明确
- **is_final 后续 chunk 被忽略**：正常行为

---

## 18. RemoteBackend 死代码清理（1 项 P1）

### 18.1 问题清单
- **4.25** [remote_backend.h:66-67](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\remote\remote_backend.h#L66)、[remote_backend.cpp:47-48](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\remote\remote_backend.cpp#L47) `m_retry_count`/`m_retry_delay_ms` 从 ConfigManager 读取但从不使用
- 重试实际在 Client 里用 Client 自己的字段

### 18.2 修复方案

**选项 A（推荐）**：删除 `m_retry_count`/`m_retry_delay_ms` 成员与读取代码。

```cpp
// remote_backend.h 删除：
// int m_retry_count = 3;
// int m_retry_delay_ms = 1000;

// remote_backend.cpp:47-48 删除：
// m_retry_count = cfg.get_or<int>("backend.retry_count", 3);
// m_retry_delay_ms = cfg.get_or<int>("backend.retry_delay_ms", 1000);
```

**选项 B**：让 RemoteBackend 实际使用这两个字段（自己重试）。但 Client 层已重试，重复重试会造成指数级延迟。不推荐。

### 18.3 风险与缓解
- **配置项 `backend.retry_count` 失效**：用户文档需说明用 `client.retry_count`。**缓解**：在 ConfigManager 中添加 deprecated 警告

---

## 19. RemoteBackend::submit_completion 在飞请求检查修复（1 项 P1）

### 19.1 问题清单
- **4.26** [remote_backend.cpp:102-139](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\remote\remote_backend.cpp#L102) 直接覆盖 `m_active_reader`，旧 reader 被覆盖但 HTTP 仍跑
- `m_active_reader` 无锁保护，与 `interrupt` 并发竞态

### 19.2 修复方案

#### 19.2.1 检查在飞请求

```cpp
std::unique_ptr<IStreamReader> RemoteBackend::submit_completion(const CompletionRequest& request) {
#ifdef WORKX_HAS_CURL
    if (!m_ready.load() || !m_adapter || !m_http_client) {
        return nullptr;
    }

    // 新增：检查是否已有在飞请求
    std::lock_guard<std::mutex> lock(m_active_mutex);  // 新增 mutex
    if (m_active_reader) {
        // 已有在飞请求，拒绝新请求
        return nullptr;
    }

    m_generating.store(true);
    // ... 原逻辑
```

#### 19.2.2 新增 mutex 保护 m_active_reader

```cpp
// remote_backend.h 新增：
private:
    mutable std::mutex m_active_mutex;
    std::shared_ptr<SSEStreamReader> m_active_reader;
```

#### 19.2.3 interrupt 也加锁

```cpp
void RemoteBackend::interrupt() {
    std::lock_guard<std::mutex> lock(m_active_mutex);
    if (m_active_reader) {
        m_active_reader->cancel();
        if (m_http_client) {
            m_http_client->cancel_stream(m_active_reader.get());
        }
        m_active_reader.reset();
    }
    m_generating.store(false);
}
```

#### 19.2.4 on_complete 回调也加锁

```cpp
m_http_client->async_post_stream(
    url, header_pairs, body, reader,
    [this]() {
        std::lock_guard<std::mutex> lock(m_active_mutex);
        m_generating.store(false);
        m_active_reader.reset();
    },
    m_config.timeout_ms);
```

### 19.3 风险与缓解
- **死锁风险**：on_complete 在 HttpClient poll 线程触发，与 submit_completion 主线程争锁。**缓解**：锁范围最小化，仅保护 m_active_reader 读写
- **测试**：测试「连续两次 submit_completion → 第二次返回 nullptr」「interrupt 期间 submit_completion → 拒绝」

---

## 20. Client::regenerate 检查 generating 修复（1 项 P1）

### 20.1 问题清单
- **4.27** [client.cpp:508-513](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\client.cpp#L508) 生成中调用会与后台 push_back 竞态

### 20.2 修复方案

```cpp
void Client::regenerate() {
    if (m_generating.load()) {
        // 生成中拒绝 regenerate
        if (m_publish_events) {
            EventBus::instance().publish_async(StreamErrorEvent{
                .session_id = "client",
                .message = "Still generating, cannot regenerate",
                .retryable = true
            });
        }
        return;
    }
    std::lock_guard<std::mutex> lock(m_messages_mutex);  // 阶段 1 已加
    if (!m_messages.empty() && m_messages.back().role == ChatMessage::Role::Assistant) {
        m_messages.pop_back();
    }
}
```

### 20.3 风险与缓解
- **无风险**：纯防御性检查

---

## 21. 重试退避溢出修复（1 项 P1）

### 21.1 问题清单
- **4.28** [client.cpp:197, 308](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\client.cpp#L197) `1 << attempt` 在 attempt=30+ 时 int 溢出（UB）

### 21.2 修复方案

```cpp
// 修改前：
// int delay = m_retry_delay_ms * (1 << attempt);

// 修改后：用 64 位 + 上限
int64_t delay = static_cast<int64_t>(m_retry_delay_ms) * (1LL << attempt);
// 上限 60 秒，避免长退避卡死
constexpr int64_t MAX_DELAY_MS = 60000;
if (delay > MAX_DELAY_MS) delay = MAX_DELAY_MS;
```

同样修改 [client.cpp:308](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\client.cpp#L308)。

### 21.3 风险与缓解
- **无风险**：纯算术修复

---

## 22. 阶段 3 实施顺序

建议按依赖关系分批实施：

### 批次 1：OpenAI 适配器修复（独立）
1. OpenAI stream_options + usage（第 4 节）
2. OpenAI 多 tool_call（第 6 节）
3. OpenAI content_filter（第 7 节）
4. OpenAI assistant content=null（第 11 节）
5. OpenAI tool_call_id 校验（第 12 节）
6. OpenAI error 事件处理（第 17 节）

### 批次 2：Anthropic 适配器修复（独立）
7. Anthropic operator[] → value（第 8 节）
8. Anthropic reasoning_content 结构化（第 9 节）
9. Anthropic 多 tool_result 合并（第 10 节）
10. Anthropic 多 system_prompt 拼接（第 13 节）
11. Anthropic message_delta input_tokens（第 14 节）

### 批次 3：HTTP 与 RemoteBackend（依赖阶段 1）
12. HTTP 流式超时（第 1 节）
13. async_post_stream 失败处理（第 15 节）
14. HTTP 错误响应体保留（第 16 节）
15. cancel_stream const 删除（第 3 节）
16. RemoteBackend list_models 分发（第 5 节）
17. RemoteBackend 死代码清理（第 18 节）
18. RemoteBackend submit_completion 在飞检查（第 19 节）

### 批次 4：Client 层（依赖批次 1-3）
19. chat 历史回滚（第 2 节）
20. Client::regenerate 检查（第 20 节）
21. 重试退避溢出（第 21 节）

### 批次 5：验证
22. 编译通过
23. 单元测试（如有）
24. 手动测试：OpenAI/Anthropic 真实 API 调用
25. 手动测试：错误重试、流式中断、tool_calls 多轮

---

## 23. 验证清单

阶段 3 完成后需验证：

### 23.1 编译
- [ ] 编译无 warning（MSVC /W4）
- [ ] 无链接错误（cancel_stream const 删除后调用方正常）

### 23.2 OpenAI 适配器
- [ ] 流式响应正常显示 token 数（prompt_tokens + generated_tokens）
- [ ] 多 tool_call 流式响应正常解析（如「读文件 + 写文件」并行调用）
- [ ] content_filter 触发时正常终止流，显示提示
- [ ] assistant with tool_calls 的 content 字段为 null
- [ ] Tool 消息 tool_call_id 空时跳过，不发送 400
- [ ] 流式 error 事件显示错误信息，不卡住

### 23.3 Anthropic 适配器
- [ ] 缺字段时不崩溃（operator[] 改 value 后）
- [ ] reasoning_content 作为 thinking block 单独输出，不混入 text
- [ ] 多个 tool_result 合并到同一 user 消息（API 不返回 400）
- [ ] 多条 system_prompt 拼接（不丢失前几条）
- [ ] message_delta 解析 input_tokens（prompt_tokens 不为 0）
- [ ] list_models 返回内置模型列表（不报 404）

### 23.4 HTTP 与 RemoteBackend
- [ ] 长流式响应（60s+）不被中途切断
- [ ] 断网 30s 触发空闲超时
- [ ] curl 初始化失败时 reader 收到错误，不无限阻塞
- [ ] HTTP 4xx/5xx 错误响应体保留在上层错误信息中
- [ ] 在飞请求时再次 submit_completion 返回 nullptr
- [ ] interrupt 期间 submit_completion 安全

### 23.5 Client 层
- [ ] chat 失败后历史无孤儿 user 消息
- [ ] 生成中调用 regenerate 被拒绝
- [ ] 重试 30+ 次不崩溃（退避不溢出）

---

## 24. 待审批事项

请审阅本 Plan 并确认：

1. **HTTP 流式超时改空闲超时**（第 1 节）是否合适？timeout_ms 默认值是否调大到 120s？

2. **chat 失败回滚 user 消息**（第 2 节）是否符合预期？还是希望保留 user 消息让用户看到上次输入？

3. **cancel_stream const 重载**（第 3 节）确认删除（选项 A）？还是补充 const 实现（选项 B）？

4. **Anthropic API 版本升级到 2025-01-01**（第 9 节）是否可接受？这会影响 thinking block 等特性。

5. **OpenAI tool_calls 多增量处理**（第 6 节）选方案 A（保守）还是方案 B（StreamChunk 改 vector）？方案 B 改动大但彻底。

6. **StreamChunk 新增 error_message 字段**（第 17 节）是否本次做？还是用 content_delta 塞错误信息（保守）？

7. **RemoteBackend 死代码清理**（第 18 节）选选项 A（删除）还是选项 B（启用）？

8. **IProviderAdapter 新增 get_models_endpoint/get_builtin_models 虚函数**（第 5 节）是否可接受？所有子类需实现。

9. **是否进入执行阶段？**

待你审批后，我将按批次顺序执行修改。整个阶段 3 预计涉及 6 个文件的修改：
- `agent/api/provider/openai_adapter.cpp` — 6 项修复
- `agent/api/provider/anthropic_adapter.cpp` — 5 项修复
- `agent/api/provider/i_provider_adapter.h` — 新增 list_models 相关虚函数
- `agent/api/remote/http_client.{h,cpp}` — 4 项修复
- `agent/api/remote/remote_backend.{h,cpp}` — 3 项修复
- `agent/api/client.cpp` — 3 项修复

无新增文件。
