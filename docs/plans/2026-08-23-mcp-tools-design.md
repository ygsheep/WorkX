# MCP 三件套接入设计 — Issue #27

- 日期：2026-08-23
- 里程碑：0.5.x 工具系统补齐（GitHub Milestone #2）
- 仓库：ygsheep/WorkX
- 基线分支：develop
- 关联 Issue：#27 [内置工具] MCPTool 为 stub 且缺 List/Read MCP Resource：无法接入 MCP 生态

## 1. 需求与目标

### 1.1 用户场景（Issue #27）

- 用户装了 GitHub MCP server，想让 AI 帮忙查 PR、创建 issue → AI 调不到
- 用户装了 Notion MCP server，想让 AI 总结文档 → AI 调不到
- 用户装了数据库 MCP server，想让 AI 跑 SQL 查询 → AI 调不到
- MCP server 暴露的 Resources（如配置文件、文档库）也无法被 AI 读取

### 1.2 交付物（三件套）

| 工具 | 职责 | 输入 |
|------|------|------|
| `MCPTool`（补完） | 调用已连接 MCP server 的工具 | `{server, tool, input}` |
| `ListMcpResourcesTool` | 列出某 MCP server 暴露的资源列表 | `{server?}` |
| `ReadMcpResourceTool` | 读取指定 MCP 资源内容 | `{server, uri}` |

三者配合实现「调用 MCP 工具 + 读取 MCP 资源」的完整生态接入。优先级 P1（不阻塞基础使用，但严重影响扩展性）。

## 2. MCP 2.0 规范调研结论

MCP 2.0 规范已于 **2026-07-28 正式发布**，是协议启动以来最大修订。与 1.x（2025-11-25）相比的关键变化：

| 维度 | 1.x（2025-11-25） | 2.0（2026-07-28） |
|------|-------------------|-------------------|
| 会话模型 | 有状态，`initialize`/`initialized` 握手 + `Mcp-Session-Id` | **无状态**，移除握手与会话头 |
| 能力协商 | 握手时协商 | 每请求 `_meta` 携带协议版本/能力；新增可选 `server/discover` RPC |
| 传输 | stdio / Streamable HTTP / HTTP+SSE | stdio / Streamable HTTP（HTTP+SSE 废弃） |
| 缓存 | 无 | `tools/list`、`resources/list`、`resources/read` 响应新增 `ttlMs`/`cacheScope` |
| 批处理 | 曾支持后移除 | 不支持 JSON-RPC batch |
| 认证 | OAuth 2.0 | 强化 OAuth 2.0/OIDC 对齐，RFC 9207 `iss` 校验，DCR → CIMD |

官方无 C++ SDK（Tier 1 为 TypeScript/Python/Go/C#），需自研轻量 client。

### 2.1 对设计的影响

1. **必须自适应协议版本**：生态中绝大多数 server 仍是 1.x（依赖 `initialize`），2.0 刚发布。client 需先试 `server/discover`（2.0），失败回退 1.x `initialize` 握手。
2. **传输优先 stdio**：stdio 是跨版本最稳定的传输，且无需认证；Streamable HTTP 作为 P2 增强。
3. **无 batch**：请求/响应按 JSON-RPC 2.0 单条消息处理，无需批处理逻辑。

## 3. 现状分析

### 3.1 已具备

| 能力 | 位置 | 说明 |
|------|------|------|
| `ITool` 接口 | `src/agent/tool/itool.h` | `name/description/prompt/input_schema/call` + `check_permissions/validate_input` |
| `ToolRegistry` | `src/agent/tool/registry.h` | 静态注册，`register_tool(shared_ptr<ITool>)` |
| 注册入口 | `src/agent/factory.cpp::register_builtin_tools()` | 未注册 MCPTool |
| `IConfigManager` | `src/core/config/i_config_manager.h` | 已注入 `ToolContext.config_manager_ptr` |
| `HttpClient` | `src/agent/api/remote/http_client.h` | 含 SSRF 防护（`set_block_private_ips`） |
| `subprocess::exec()` | `src/core/process/subprocess.h` | 一次性同步执行，**不支持持久子进程** |
| MCPTool stub | `src/agent/tool/MCPTool/mcp_tool.{h,cpp}` | `call()` 返回 `NotImplemented`，input_schema 已是 `{server, tool, input}` 分发形 |

### 3.2 缺口

1. **无 MCP client 基础设施**：无 client 类、无传输层、无协议消息结构、无配置管理。
2. **无持久子进程能力**：stdio 传输需要长驻子进程 + 双向管道，现有 `exec()` 是一次性的。
3. **MCPTool 未注册**、List/Read Resource 工具完全缺失。

## 4. 总体架构

```
┌────────────────────────────────────────────────────────────┐
│ Agent 层（C++）                                             │
│                                                            │
│  ToolRegistry                                              │
│   ├─ MCPTool（分发式：server+tool+input）                    │
│   ├─ ListMcpResourcesTool                                  │
│   └─ ReadMcpResourceTool                                   │
│         │                                                  │
│         ▼                                                  │
│  McpClientManager（连接管理，生命周期=会话）                  │
│   ├─ McpClient[github] ── StdioTransport ──► npx server-github│
│   ├─ McpClient[notion] ── HttpTransport ──► https://mcp.notion│
│   └─ ...                                                  │
│         │                                                  │
│         ▼                                                  │
│  配置：~/.workx/mcp.json（用户级）+ <cwd>/.mcp.json（项目级）│
└────────────────────────────────────────────────────────────┘
```

## 5. 详细设计

### 5.1 新增文件清单

```
src/agent/mcp/
├── mcp_types.h              # 协议消息结构
├── mcp_config.h / .cpp      # server 配置解析（标准 mcp.json 格式）
├── mcp_transport.h / .cpp   # 传输抽象 + Stdio/Http 实现
├── mcp_stdio_process.h / .cpp  # 持久子进程（双向管道，Windows/POSIX）
├── mcp_client.h / .cpp      # JSON-RPC client（协议协商 + 四类方法）
└── mcp_client_manager.h / .cpp # 连接管理 + 工具清单快照

src/agent/tool/MCPTool/mcp_tool.cpp                  # 补完 call()
src/agent/tool/ListMcpResourcesTool/list_mcp_resources_tool.h / .cpp
src/agent/tool/ReadMcpResourceTool/read_mcp_resource_tool.h / .cpp
```

### 5.2 协议消息结构（mcp_types.h）

```cpp
/// MCP server 暴露的工具信息（tools/list 响应项）
struct McpToolInfo {
    std::string name;
    std::string description;
    nlohmann::json input_schema;   // JSON Schema（2.0 支持 2020-12 全词汇表）
};

/// MCP server 暴露的资源信息（resources/list 响应项）
struct McpResourceInfo {
    std::string uri;
    std::string name;
    std::string mime_type;
    std::string description;
};

/// 资源内容（resources/read 响应项）
struct McpResourceContent {
    std::string uri;
    std::string mime_type;
    std::string text;   // 文本内容
    std::string blob;   // 二进制内容（base64）
};
```

### 5.3 配置解析（mcp_config）

**格式对齐生态**（Claude Code / VS Code 通用 `mcp.json`，用户可迁移现有配置）：

```json
{
  "mcpServers": {
    "github": {
      "command": "npx",
      "args": ["-y", "@modelcontextprotocol/server-github"],
      "env": { "GITHUB_PERSONAL_ACCESS_TOKEN": "..." }
    },
    "notion": {
      "type": "http",
      "url": "https://mcp.notion.com/mcp",
      "headers": { "Authorization": "Bearer ..." }
    }
  }
}
```

```cpp
/// 单个 server 配置
struct McpServerConfig {
    std::string name;
    // stdio
    std::string command;
    std::vector<std::string> args;
    std::map<std::string, std::string> env;
    // http
    std::string url;
    std::map<std::string, std::string> headers;
    bool is_http() const { return !url.empty(); }
};

/// 配置加载：用户级 ~/.workx/mcp.json + 项目级 <cwd>/.mcp.json（项目级优先合并）
ResultV2<std::vector<McpServerConfig>> load_mcp_configs(
    const std::filesystem::path& user_config_dir,
    const std::filesystem::path& cwd);
```

### 5.4 传输层（mcp_transport）

```cpp
/// 传输抽象：发送 JSON-RPC 消息并同步等待响应
class McpTransport {
public:
    virtual ~McpTransport() = default;
    virtual ResultV2<void> start() = 0;
    virtual void stop() = 0;
    /// 发送请求（单条 JSON-RPC 消息），同步阻塞等待匹配 id 的响应
    virtual ResultV2<nlohmann::json> send_request(
        const nlohmann::json& msg, int timeout_ms) = 0;
    /// 发送通知（无需响应）
    virtual ResultV2<void> send_notification(const nlohmann::json& msg) = 0;
};

/// stdio 传输：spawn 持久子进程，stdin 写 / stdout 读（按行解析 JSON）
class StdioMcpTransport : public McpTransport { ... };

/// Streamable HTTP 传输：POST JSON-RPC 到 url（复用 HttpClient，SSRF 防护）
class HttpMcpTransport : public McpTransport { ... };
```

**stdio 传输依赖持久子进程**（M1 核心工作量）。现有 `subprocess::exec()` 是一次性同步执行，需新增 `McpStdioProcess`：

```cpp
/// 持久子进程：启动后保持运行，支持双向管道读写（Windows/POSIX）
class McpStdioProcess {
public:
    ResultV2<void> start(const std::string& cmd,
                         const std::vector<std::string>& args,
                         const std::map<std::string, std::string>& env);
    void stop();   // 关闭 stdin + 终止进程（超时后强杀）
    /// 写入一行（stdin），返回是否成功
    ResultV2<void> write_line(const std::string& line);
    /// 阻塞读取一行（stdout），EOF 返回空
    ResultV2<std::string> read_line(int timeout_ms);
    bool is_alive() const;
private:
    // Windows: CreateProcessW + 双向匿名管道 + 读线程
    // POSIX:   fork + execvp + pipe + poll
};
```

实现要点：
- **Windows**：`CreateProcessW` + 两条匿名管道（stdin/stdout），stdout 读线程按行缓冲，`WaitForSingleObject` 检测退出。
- **POSIX**：`fork` + `execvp` + `pipe`，`poll` 检测可读/超时/退出。
- 超时/取消时保证 kill 子进程（防孤儿进程），对齐现有 `exec()` 的防护策略。

### 5.5 MCP Client（mcp_client）

```cpp
class McpClient {
public:
    /// 连接：协议协商（server/discover → initialize 回退）
    ResultV2<void> connect(const McpServerConfig& cfg, int timeout_ms = 15000);
    void disconnect();

    bool is_connected() const;
    const std::string& name() const;
    const std::string& protocol_version() const;

    // 工具
    ResultV2<std::vector<McpToolInfo>> list_tools();
    ResultV2<nlohmann::json> call_tool(const std::string& tool_name,
                                       const nlohmann::json& args);

    // 资源
    ResultV2<std::vector<McpResourceInfo>> list_resources();
    ResultV2<std::vector<McpResourceContent>> read_resource(const std::string& uri);

private:
    ResultV2<nlohmann::json> request(const std::string& method,
                                     const nlohmann::json& params,
                                     int timeout_ms);
    ResultV2<void> notify(const std::string& method, const nlohmann::json& params);

    std::unique_ptr<McpTransport> m_transport;
    std::string m_name;
    std::string m_protocol_version;  // 协商结果（"2025-11-25" / "2026-07-28"）
    bool m_connected = false;
    int m_next_id = 1;
};
```

**协议协商流程（2.0 兼容）**：

```
connect():
  ① 发送 server/discover（2.0 新增，可选 RPC）
     ├─ 有效响应（含 protocolVersion）→ 2.0 无状态模式
     │    └─ 后续每请求 _meta 携带 io.modelcontextprotocol/protocolVersion
     └─ MethodNotFound / 超时 → 回退 1.x：
          ② initialize（携带 supportedProtocolVersions）
             ├─ 成功 → notifications/initialized → 就绪
             └─ UnsupportedProtocolVersionError → 用返回版本重试 initialize
```

**JSON-RPC 消息格式**：

```json
// 请求
{ "jsonrpc": "2.0", "id": 1, "method": "tools/call",
  "params": { "name": "create_issue", "arguments": { ... } } }
// 响应
{ "jsonrpc": "2.0", "id": 1,
  "result": { "content": [ { "type": "text", "text": "..." } ],
              "isError": false } }
// 错误
{ "jsonrpc": "2.0", "id": 1,
  "error": { "code": -32601, "message": "Method not found" } }
```

`request()` 实现要点：
- 自增 `id`，发送后同步等待匹配 id 的响应（带超时）。
- 收到 `error` 时返回 `Error`（映射 `-32601 MethodNotFound`、`-32602 InvalidParams` 等）。
- 忽略 `notifications`（无 id）与不匹配 id 的消息。
- `tools/call` 结果规范化：`content[]` 中 `type=text` 拼接为文本，`type=image`/`resource` 标注说明。

### 5.6 连接管理（mcp_client_manager）

```cpp
class McpClientManager {
public:
    /// 从配置加载并连接所有 server（会话启动时调用，失败不阻断会话）
    void load_and_connect(IConfigManager& cfg, const std::filesystem::path& cwd);

    /// 按 server 名获取 client（不存在返回 nullptr）
    std::shared_ptr<McpClient> get_client(const std::string& name) const;
    std::vector<std::shared_ptr<McpClient>> clients() const;

    /// 已连接 server 的工具清单快照（供 MCPTool prompt 注入）
    std::string describe_servers() const;

    /// 已连接 server 名列表（供 ListMcpResourcesTool 过滤提示）
    std::vector<std::string> server_names() const;
private:
    std::map<std::string, std::shared_ptr<McpClient>> m_clients;
};
```

- 连接失败仅记录日志，不阻断会话启动（server 可后续重连）。
- 线程安全：`std::mutex` 保护 `m_clients`（工具可能并发调用）。

### 5.7 三个工具

**MCPTool（补完，分发式）**

```cpp
class MCPTool : public ITool {
public:
    explicit MCPTool(std::shared_ptr<McpClientManager> manager);
    // name: "MCP"
    // input_schema: {server, tool, input}（保留现有结构）
    // prompt(): 动态注入已连接 server + 工具清单（describe_servers()）
    // call(): manager->get_client(server)->call_tool(tool, input)
    // check_permissions(): Default 模式 AskUser 确认外部工具调用
};
```

**ListMcpResourcesTool（新增）**

```cpp
class ListMcpResourcesTool : public ITool {
public:
    explicit ListMcpResourcesTool(std::shared_ptr<McpClientManager> manager);
    // name: "ListMcpResourcesTool"
    // input_schema: {server?}（可选过滤）
    // is_read_only(): true
    // call(): 列出所有（或指定）server 的 resources（uri/name/mimeType/description）
};
```

**ReadMcpResourceTool（新增）**

```cpp
class ReadMcpResourceTool : public ITool {
public:
    explicit ReadMcpResourceTool(std::shared_ptr<McpClientManager> manager);
    // name: "ReadMcpResourceTool"
    // input_schema: {server, uri}
    // is_read_only(): true
    // call(): 读取资源内容（text 或 base64 blob）
};
```

### 5.8 权限与安全

| 工具 | 权限策略 |
|------|----------|
| MCPTool | Default 模式 AskUser 确认（对齐 WebFetchTool 非白名单模式）；Bypass 放行 |
| ListMcpResourcesTool | 只读，默认放行 |
| ReadMcpResourceTool | 只读，默认放行 |

- **HttpTransport**：复用 `HttpClient::set_block_private_ips(true)` SSRF 防护。
- **stdio**：命令来自用户显式配置文件，无需额外确认。
- **超时**：所有 MCP 请求默认 15s 超时（对齐项目 HTTP 约束）。

### 5.9 装配与注册

`src/agent/factory.cpp`：

```cpp
// create_session() 内，注册内置工具之前：
auto mcp_manager = std::make_shared<tool::McpClientManager>();
mcp_manager->load_and_connect(cfg, fs::current_path());

// register_builtin_tools() 增加：
registry.register_tool(std::make_shared<tool::MCPTool>(mcp_manager));
registry.register_tool(std::make_shared<tool::ListMcpResourcesTool>(mcp_manager));
registry.register_tool(std::make_shared<tool::ReadMcpResourceTool>(mcp_manager));
```

`src/agent/CMakeLists.txt` 的 `target_sources(workx_agent PRIVATE ...)` 显式加入：

```cmake
mcp/mcp_config.cpp
mcp/mcp_transport.cpp
mcp/mcp_stdio_process.cpp
mcp/mcp_client.cpp
mcp/mcp_client_manager.cpp
tool/ListMcpResourcesTool/list_mcp_resources_tool.cpp
tool/ReadMcpResourceTool/read_mcp_resource_tool.cpp
```

### 5.10 系统提示词注入

`MCPTool::prompt()` 动态返回 `describe_servers()` 生成的清单，让模型知道可调用哪些 server/tool。示例：

```
MCP 工具：调用已连接的 MCP server 工具。
当前已连接 server：
- github（工具：create_issue, list_pulls, ...）
- notion（工具：search, append_to_page, ...）
用法：{"server": "github", "tool": "create_issue", "input": {...}}
```

## 6. 实施里程碑

| 里程碑 | 内容 | 验收标准 |
|--------|------|----------|
| **M1（P0）** | mcp_types + mcp_config + mcp_stdio_process + StdioTransport + McpClient（1.x initialize + 四类方法） | 单元测试：配置解析、JSON-RPC 编解码、stdio 进程读写 |
| **M2（P1）** | 三件套实现 + factory 注册 + prompt 注入 + 单元测试 | 三件套可调用 mock MCP server；构建通过 |
| **M3（P2）** | server/discover 2.0 协商 + HttpTransport + 集成测试 | mock 2.0 server 走无状态模式；HTTP server 可连 |

## 7. 测试计划

### 7.1 单元测试（tests/unit/agent/）

| 测试 | 覆盖 |
|------|------|
| `mcp_config_test` | mcp.json 解析（stdio/http/缺失字段/非法 JSON）、用户级+项目级合并 |
| `mcp_jsonrpc_test` | 请求/响应/错误消息编解码、id 匹配、超时 |
| `mcp_protocol_test` | 协商决策（discover 成功→2.0；MethodNotFound→1.x initialize）、UnsupportedProtocolVersionError 重试 |
| `mcp_stdio_process_test` | 持久子进程启动/写入/读取/超时/终止（用 `cat`/`python -c` 作为 mock） |
| `mcp_tools_test` | 三件套调用（注入 mock McpClientManager） |

### 7.2 集成测试（tests/integration/）

- 用 Python/Node 脚本实现 mock MCP server（stdio），验证：
  - `initialize` 握手 → `tools/list` → `tools/call` 全链路
  - `resources/list` → `resources/read` 全链路
  - 错误路径（tool 不存在、server 名不存在）

## 8. 风险与权衡

| 风险/权衡 | 影响 | 缓解 |
|-----------|------|------|
| stdio 持久子进程是新增能力 | M1 核心工作量 | 参考现有 `exec()` 的进程管理经验；Windows/POSIX 双实现 |
| 分发式 vs 动态注册 | 模型需依赖 prompt 提示工具清单 | 分发式简单可控；prompt 注入完整清单；后续可评估动态注册 |
| MCP 2.0 生态未成熟 | 2.0 server 少 | 1.x 兼容优先，2.0 协商作为 P2 渐进增强 |
| 外部 server 不可控 | 工具可能长时间无响应 | 统一 15s 超时 + 取消传播 |
| 配置含密钥（token） | 安全风险 | 复用现有 secret_scanner；文档提示环境变量注入 |

## 9. 参考

- MCP 2.0 规范：https://modelcontextprotocol.io/specification/2026-07-28
- MCP 变更日志：https://modelcontextprotocol.io/specification/2026-07-28/changelog
- example/cc 参考实现：`example/cc/services/mcp/` + `example/cc/tools/{MCPTool,ListMcpResourcesTool,ReadMcpResourceTool}/`
- 现有工具范式：`src/agent/tool/WebFetchTool/web_fetch_tool.cpp`（权限 + 调用 + 错误返回）
