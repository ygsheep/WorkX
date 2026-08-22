# WebSearchTool — 网页搜索工具

按关键词搜索互联网，返回标题、URL、摘要的结构化列表，供 LLM 获取时效性信息或验证资料出处。支持 Tavily（需 API Key）、SearXNG（免 Key）、Bing（免 Key）三档 Provider，自动降级兜底。

## 目录结构

```
src/agent/tool/
└── WebSearchTool/
    ├── web_search_tool.h      # 接口声明（v1.1.0）
    ├── web_search_tool.cpp    # 实现（v1.1.0）
    └── README.md              # 本文档
```

## 工具元数据

| 字段 | 值 |
|------|-----|
| name | `WebSearch` |
| description | 按关键词搜索互联网，返回标题、URL、摘要的结构化列表。用于获取时效性信息或验证资料出处。 |
| namespace | `agent::tool` |
| 基类 | `ITool` |
| 只读 | 是（`is_read_only() == true`，无副作用） |
| 同步/异步 | 同步（返回 `ResultV2<ToolResult>`） |

## 输入 Schema

```json
{
  "type": "object",
  "properties": {
    "query":        { "type": "string",  "description": "搜索关键词（必填）。建议用英文或中英混合的关键词，不必写整句。" },
    "num_results":  { "type": "integer", "minimum": 1, "maximum": 20, "default": 8, "description": "返回条数（1-20，默认 8）" },
    "search_depth": { "type": "string",  "enum": ["basic", "advanced"], "default": "basic", "description": "深度：basic 快（默认），advanced 抓取首条详情更准但更慢/更贵。" }
  },
  "required": ["query"],
  "additionalProperties": false
}
```

### 字段说明

| 字段 | 类型 | 必填 | 默认值 | 说明 |
|------|------|------|--------|------|
| `query` | string | 是 | — | 搜索关键词，建议英文或中英混合关键词，不必写整句 |
| `num_results` | int | 否 | `8` | 返回条数，范围 `[1, 20]`（越界自动裁剪） |
| `search_depth` | string | 否 | `basic` | `basic` 快（默认）；`advanced` 抓取首条详情更准但更慢/更贵（仅 Tavily 生效） |

## 输出格式

统一为 `[index] 标题 · URL` + 缩进摘要的文本，便于 LLM 阅读：

```
# WebSearch 结果: <query>
Provider: Bing (免 Key, max=5)

---

[1] 知乎 - 有问题，就会有答案 · https://www.zhihu.com/
    知乎，中文互联网高质量的问答社区和创作者聚集的原创内容平台，于 2011 年 1 月正式上线…

[2] 博客园文章 · https://www.cnblogs.com/x/p/1.html
    这是一篇 C++ 教程摘要…
```

- 每条摘要截断至 600 字符，超出追加 `…`
- 摘要内换行转为 `\n` + 4 空格缩进
- 无结果时返回占位提示：`未找到相关搜索结果。建议更换关键词、尝试英文关键词或缩小范围。`

## Provider 链

按配置 `web.search.provider` 选择，自动降级兜底：

```
tavily（默认，需 API Key）
   │  无 Key / 限流 / 网络失败
   ▼
searxng（免 Key，公共实例）
   │  超时 / 不可达
   ▼
bing（免 Key，最终兜底，公共可达）
```

| Provider | 请求 | 需 Key | 说明 |
|----------|------|--------|------|
| `tavily` | `POST https://api.tavily.com/search` | 是 | 质量最高，支持 `search_depth` |
| `searxng` | `GET {searxng_url}/search?q=..&format=json` | 否 | 公共实例常超时，建议自建或换实例 |
| `bing` | `GET https://www.bing.com/search?q=..` | 否 | 解析 HTML 结果页，公共可达，最终兜底 |

## 执行管道

```mermaid
flowchart TD
    START([call input, ctx]) --> PARSE[1. 解析参数<br/>query 必填 / num_results 裁剪 / depth]
    PARSE --> PROVIDER[2. 解析配置<br/>provider + api_key + searxng_url]
    PROVIDER --> BRANCH{3. provider 分支}
    BRANCH -->|bing| BING[search_bing]
    BRANCH -->|searxng| SX[search_searxng]
    SX -->|失败| BING
    BRANCH -->|tavily| HASKEY{4. 有 API Key?}
    HASKEY -->|否| SX
    HASKEY -->|是| TAV[search_tavily]
    TAV -->|失败| SX
    BING --> FMT[5. 格式化输出<br/>[index] 标题 · URL + 摘要]
    SX --> FMT
    TAV --> FMT
    FMT --> DONE([ToolResult::ok])
```

## 关键特性

### 1. 免 Key 搜索兜底（#25）

未配置 Tavily API Key 时自动降级到 SearXNG / Bing，开箱即用：

- **SearXNG**：`GET {url}/search?q={query}&format=json&safesearch=0`，query 做百分号 URL 编码（UTF-8），响应 `results[]` 结构与 Tavily 兼容，复用同一格式化逻辑
- **Bing**：`GET https://www.bing.com/search?q={query}&count={n}&setlang=zh-hans&mkt=zh-CN`，解析 HTML 结果页

### 2. Bing HTML 结果解析

- 定位 `<li class="b_algo">` 块，块内 `<h2><a href="...">标题</a></h2>` 取标题 + URL，`<p>` 取摘要
- 标题/摘要先剥离 HTML 标签，再解码实体（`&#NNN;` / `&#xHH;` / 常见命名实体如 `&amp;` `&ensp;` `&middot;`）
- 无结果判断依赖 `b_algo` 计数为 0 或文案「没有与此相关的结果」；**不能**用 `b_no` 子串（会误匹配页面 JS 的 `#b_notificationContainer`）
- 请求携带标准 Chrome UA + `Accept-Language: zh-CN`，避免被反爬拦截；`www.bing.com` 会 302 到 `cn.bing.com`（HttpClient 自动跟随）

### 3. 权限检查（#25）

| 场景 | 行为 |
|------|------|
| Bypass 模式 | 全放行 |
| 普通搜索词 | 放行（搜索本身只读安全） |
| 命中敏感关键词 | AskUser 确认，拒绝则返回 `PermissionDenied` |

敏感关键词覆盖两类：

- **内网地址/路径**：`10.` `192.168.` `172.16-19.` `127.0.0.1` `localhost` `169.254.` `内网`
- **凭据/敏感信息**：`password` `passwd` `secret` `token` `api_key` `apikey` `private key` `ssh key` `credential` `密码` `密钥` `口令`

### 4. 纯函数（便于单测）

| 函数 | 作用 |
|------|------|
| `build_tavily_request` | 组装 Tavily 请求体，含 num_results 范围裁剪 |
| `parse_tavily_response` | Tavily JSON → 文本（answer 置顶、跳过脏条目、摘要截断） |
| `parse_searxng_response` | SearXNG JSON → 文本（复用 Tavily 格式化） |
| `build_searxng_url` | 构造 SearXNG 查询 URL（query URL 编码） |
| `build_bing_url` | 构造 Bing 查询 URL（query URL 编码 + count/mkt） |
| `parse_bing_response` | Bing HTML → 文本（b_algo 解析 + 实体解码） |

## 配置项

| 配置键 | 类型 | 默认值 | 环境变量 | 说明 |
|--------|------|--------|----------|------|
| `web.search.provider` | string | `tavily` | `WORKX_SEARCH_PROVIDER` | 搜索 Provider：`tavily` / `searxng` / `bing` |
| `web.search.tavily_api_key` | string | `""` | `TAVILY_API_KEY` | Tavily API Key（持久化于 `~/.workx/config.json`） |
| `web.search.searxng_url` | string | `https://searx.be` | `WORKX_SEARXNG_URL` | SearXNG 实例地址（免 Key 兜底，可换自建实例） |

**API Key 来源优先级**：
1. `AppConfig web.search.tavily_api_key`（持久化配置）
2. 环境变量 `TAVILY_API_KEY`
3. 环境变量 `WORKX_TAVILY_API_KEY`

## 错误处理

所有错误通过 `ResultV2<ToolResult>::err` 返回，不抛异常。

| 错误场景 | 错误码 | 错误信息 |
|---------|--------|---------|
| query 缺失/非字符串 | `InvalidInput` | `WebSearch 需要字符串参数 query` |
| query 为空 | `InvalidInput` | `WebSearch query 不能为空` |
| 网络请求失败 | `NetworkDisconnected` | `WebSearch(Tavily/SearXNG/Bing) 请求失败: <msg>` |
| HTTP 429 限流 | `HttpRateLimited` | `被限流 (HTTP 429)`，SearXNG 提示可换实例 |
| HTTP 4xx/5xx | `HttpError` | `返回 HTTP <code>: <body>`（Tavily 请求体中的 key 已脱敏为 `***`） |
| JSON 解析失败 | `InternalError` | `响应 JSON 解析失败: <what>` |

## 输入验证

`validate_input()` 在 `call()` 之前由 `ToolExecutor` 调用，提前拦截非法输入：

| 检查项 | 失败信息 |
|--------|---------|
| `query` 缺失或非字符串 | `WebSearch 需要字符串参数 query` |
| `query` 为空字符串 | `WebSearch query 不能为空` |
| `num_results` 越界 | 自动裁剪到 `[1, 20]`（不报错） |

## 使用示例

### 基本搜索

```cpp
#include "agent/tool/WebSearchTool/web_search_tool.h"

using namespace agent::tool;

WebSearchTool tool;
ToolContext ctx;
ctx.permission_mode = PermissionMode::BypassPermissions;

nlohmann::json input = {
    {"query", "nlohmann json install"},
    {"num_results", 5}
};

auto r = tool.call(input, ctx);
if (r.is_ok()) {
    std::cout << r.value().text << std::endl;
}
```

### 指定 Provider（免 Key 用 Bing）

```cpp
ConfigManager::instance().set(keys::WEB_SEARCH_PROVIDER, std::string("bing"));
// 或环境变量 WORKX_SEARCH_PROVIDER=bing
```

### 配置 Tavily Key（持久化）

```cpp
ConfigManager::instance().set(keys::WEB_SEARCH_TAVILY_KEY, std::string("tvly-xxxx"));
// 或环境变量 TAVILY_API_KEY=tvly-xxxx
```

## 设计决策

| 决策 | 理由 |
|------|------|
| Provider 链自动降级 | 无 Key 也能用；Tavily 限流/失败不中断搜索，最终兜底 Bing 公共可达 |
| Bing 作为最终兜底 | 公共 SearXNG 实例常超时/不可达；Bing 无需 Key 且国内可达 |
| 输出纯文本 `[index] 标题 · URL` | 与 Tavily/SearXNG/Bing 三 Provider 统一，LLM 易解析，后续可 WebFetch 跟进 |
| 摘要截断 600 字符 | 避免单条撑爆上下文 |
| 敏感词 AskUser | 搜索本身只读安全，仅当关键词疑似内网/凭据时确认，防信息泄露 |
| `additionalProperties: false` | 严格 schema 校验，防止 LLM 传入未定义字段 |
| 纯函数静态化 | 便于无网络单测（URL 构造、响应解析） |
| Bing 正则用自定义原始串分隔符 | 模式内 `([^"]+)"` 含 `)"` 会提前终止 `R"(...)"`，改用 `R"_h2(...)_h2"` |

## 依赖

- C++20（`std::format`）
- 标准库：`<regex>`、`<sstream>`、`<algorithm>`、`<cctype>`、`<cstdlib>`、`<vector>`
- 项目内部：`itool.h`、`context.h`、`http_client.h`、`app_config.h`、`permission_ask.h`、`error.h`、`logger.h`
- 第三方：`nlohmann::json`、libcurl（经 `HttpClient` 封装）

## 实测结果（2026-08）

| 场景 | 结果 |
|------|------|
| Bing 免 Key 搜索（中文关键词 / `site:` 语法） | ✅ 正常返回结构化结果 |
| 博客园文章 → Markdown（配合 WebFetch） | ✅ 成功 |
| 稀土掘金文章 → Markdown（配合 WebFetch） | ✅ 成功 |
| 知乎抓取 | ❌ 403 — zse-ck JS 反爬挑战，纯 HTTP 客户端无法绕过（平台限制） |
| 知识星球首页抓取 | ⚠️ JS 渲染 SPA，直接抓取无正文（平台限制） |

> 知乎、知识星球内容虽无法直接抓取，但搜索仍能返回其链接与摘要，模型可据此引导用户。

## 后续规划

- [x] Tavily 搜索（需 Key）
- [x] SearXNG 免 Key 兜底（URL 编码 + JSON 解析）
- [x] Bing 免 Key 最终兜底（HTML 解析 + 实体解码）
- [x] 权限检查（敏感词 AskUser）
- [x] 配置键 `web.search.*` 与环境变量映射
- [x] 单测（URL 构造 / 响应解析 / 权限 / 配置）与 `web_live_probe` 实测探针
- [ ] 页面缓存（重复查询命中缓存，减少请求）
- [ ] Tavily → Serper → SearXNG 多 Provider 链完善（`serper` 已预留）
- [ ] 搜索结果的 `search_depth=advanced` 抓取首条详情
