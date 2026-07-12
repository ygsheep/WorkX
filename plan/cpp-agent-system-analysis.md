# Claude Code Agent 系统架构分析

> C++ Agent 子代理系统实现参考文档
> 目录结构：`agent-core/tools/inclaude/*.h` + `agent-core/utils/inclaude/*.h`

---

## 一、整体架构概览

Claude Code 的 Agent 系统采用**分层代理架构**，支持多种代理模式：

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         Agent 系统架构                                   │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │                      主会话 (Main Session)                       │   │
│  │  ┌─────────────────────────────────────────────────────────┐   │   │
│  │  │  AgentTool (调用入口)                                    │   │   │
│  │  │  ├─ spawnSubagent() → SubagentContext                    │   │   │
│  │  │  ├─ spawnTeammate() → TeammateAgentContext               │   │   │
│  │  │  └─ forkSubagent() → ForkContext (隐式继承)              │   │   │
│  │  └─────────────────────────────────────────────────────────┘   │   │
│  └─────────────────────────────────────────────────────────────────┘   │
│                              │                                          │
│        ┌─────────────────────┼─────────────────────┐                   │
│        ▼                     ▼                     ▼                   │
│  ┌───────────────┐     ┌───────────────┐     ┌───────────────┐         │
│  │   Subagent    │     │    Fork Agent │     │   Teammate    │         │
│  │  (进程内代理)  │     │   (隐式分叉)   │     │   (Swarm成员)  │         │
│  │  ├─ in-process│     │  ├─ 继承父上下文 │     │  ├─ 进程内/外  │         │
│  │  ├─ 快速任务   │     │  ├─ 后台运行    │     │  ├─ 团队协调   │         │
│  │  └─ 结果返回   │     │  └─ 独立工作树  │     │  └─ 共享状态   │         │
│  └───────────────┘     └───────────────┘     └───────────────┘         │
│                              │                                          │
│                              ▼                                          │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │                        runAgent()                               │   │
│  │  ├─ 初始化 MCP 服务器                                           │   │
│  │  ├─ 构建系统提示词                                               │   │
│  │  ├─ 处理消息分叉                                                 │   │
│  │  ├─ 设置权限模式                                                 │   │
│  │  ├─ 创建子代理上下文                                             │   │
│  │  └─ query() 循环 → ReAct 推理                                   │   │
│  └─────────────────────────────────────────────────────────────────┘   │
│                              │                                          │
│                              ▼                                          │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │                    AgentContext (AsyncLocalStorage)              │   │
│  │  ├─ SubagentContext { agentId, parentSessionId, subagentName }  │   │
│  │  └─ TeammateAgentContext { agentId, agentName, teamName }      │   │
│  └─────────────────────────────────────────────────────────────────┘   │
│                              │                                          │
│                              ▼                                          │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │                   Agent Memory (持久化记忆)                       │   │
│  │  ├─ user scope: ~/.claude/agent-memory/                         │   │
│  │  ├─ project scope: .claude/agent-memory/                        │   │
│  │  └─ local scope: .claude/agent-memory-local/                    │   │
│  └─────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 二、代理类型与上下文系统

### 2.1 代理类型定义

| 代理类型 | 上下文类型 | 运行模式 | 用途 |
|---------|-----------|---------|------|
| **Subagent** | `SubagentContext` | 进程内同步/异步 | 快速任务委托（Explore、Plan等） |
| **Fork Agent** | `SubagentContext` | 进程内后台 | 隐式分叉，继承父上下文 |
| **Teammate** | `TeammateAgentContext` | 进程内/外 | Swarm 团队成员，有团队协调 |

### 2.2 AgentContext 设计

```typescript
type SubagentContext = {
  agentId: string                    // 代理 UUID
  parentSessionId?: string           // 父会话 ID
  agentType: 'subagent'              // 类型标识
  subagentName?: string              // 代理类型名（如 "Explore"）
  isBuiltIn?: boolean                // 是否内置代理
  invokingRequestId?: string         // 调用者请求 ID
  invocationKind?: 'spawn' | 'resume' // 调用类型
  invocationEmitted?: boolean        // 遥测标记
}

type TeammateAgentContext = {
  agentId: string                    // 完整代理 ID，如 "researcher@my-team"
  agentName: string                  // 显示名称
  teamName: string                   // 所属团队
  agentColor?: string                // UI 颜色
  planModeRequired: boolean          // 是否需要计划模式
  parentSessionId: string            // 团队负责人会话 ID
  isTeamLead: boolean                // 是否团队负责人
  agentType: 'teammate'              // 类型标识
}
```

### 2.3 AsyncLocalStorage 隔离机制

**设计原因**：

```
问题：当代理被后台化（ctrl+b）时，多个代理可以在同一进程中并发运行
方案：使用 AsyncLocalStorage 隔离每个异步执行链
结果：并发代理不会互相干扰上下文
```

---

## 三、AgentTool 执行管道

### 3.1 完整执行流程

```
AgentTool.call(input)
    │
    ├─ 1. 权限检查
    │   ├─ filterDeniedAgents() → 过滤拒绝的代理类型
    │   └─ getDenyRuleForAgent() → 获取拒绝规则
    │
    ├─ 2. 代理路由
    │   ├─ forkSubagentEnabled && !subagent_type → fork 路径
    │   ├─ isTeammate → spawnTeammate()
    │   └─ 否则 → 普通子代理路径
    │
    ├─ 3. 隔离模式处理
    │   ├─ isolation: 'worktree' → createAgentWorktree()
    │   └─ isolation: 'remote' → registerRemoteAgentTask()
    │
    ├─ 4. 同步/异步决策
    │   ├─ run_in_background || autoBackground → 异步执行
    │   └─ 否则 → 同步执行
    │
    ├─ 5. 构建上下文消息
    │   ├─ fork 路径 → buildForkedMessages()
    │   └─ 普通路径 → createUserMessage()
    │
    ├─ 6. 执行代理
    │   └─ runAgent() → query() 循环
    │
    └─ 7. 结果处理
        ├─ finalizeAgentTool() → 格式化结果
        ├─ emitTaskProgress() → 发送进度通知
        └─ renderToolResultMessage() → UI 渲染
```

### 3.2 输入/输出 Schema

**输入 Schema**：

| 字段 | 类型 | 说明 |
|-----|------|-----|
| `description` | string | 任务描述（3-5词） |
| `prompt` | string | 代理执行的任务 |
| `subagent_type` | string (optional) | 专用代理类型 |
| `model` | 'sonnet'/'opus'/'haiku' (optional) | 模型覆盖 |
| `run_in_background` | boolean (optional) | 后台运行 |
| `name` | string (optional) | 代理名称（SendMessage 寻址） |
| `team_name` | string (optional) | 团队名称 |
| `mode` | PermissionMode (optional) | 权限模式 |
| `isolation` | 'worktree'/'remote' (optional) | 隔离模式 |
| `cwd` | string (optional) | 工作目录 |

**输出 Schema**：

```typescript
type Output = 
  | { status: 'completed', prompt: string, ... }      // 同步完成
  | { status: 'async_launched', agentId: string, ... } // 异步启动
  | { status: 'teammate_spawned', ... }               // 团队成员生成
  | { status: 'remote_launched', taskId: string, ... } // 远程启动
```

---

## 四、runAgent 核心引擎

### 4.1 引擎初始化流程

```
runAgent() 初始化阶段
    │
    ├─ 1. 模型解析 → getAgentModel()
    │   ├─ 'inherit' → 继承父模型
    │   ├─ 'sonnet'/'opus'/'haiku' → 解析别名
    │   └─ Bedrock 区域前缀继承
    │
    ├─ 2. 消息分叉处理
    │   ├─ forkContextMessages → cloneFileStateCache()
    │   └─ 过滤不完整工具调用 → filterIncompleteToolCalls()
    │
    ├─ 3. 上下文精简
    │   ├─ Explore/Plan → 移除 claudeMd（节省 Token）
    │   └─ Explore/Plan → 移除 gitStatus（节省 Token）
    │
    ├─ 4. 权限模式设置
    │   ├─ agentDefinition.permissionMode → 代理定义的模式
    │   ├─ bubble → 提示冒泡到父终端
    │   └─ async → 自动拒绝权限提示
    │
    ├─ 5. 工具解析
    │   ├─ useExactTools → 使用完整工具池（fork 路径）
    │   └─ resolveAgentTools() → 过滤代理可用工具
    │
    ├─ 6. MCP 服务器初始化
    │   └─ initializeAgentMcpServers() → 合并父 + 代理专用服务器
    │
    ├─ 7. 技能预加载
    │   └─ agentDefinition.skills → 预加载技能到初始消息
    │
    ├─ 8. 创建子代理上下文
    │   └─ createSubagentContext() → 隔离/共享状态
    │
    └─ 9. 记录元数据
        └─ writeAgentMetadata() → agentType, worktreePath, description
```

### 4.2 ReAct 循环

```typescript
for await (const message of query({
  messages: initialMessages,
  systemPrompt: agentSystemPrompt,
  userContext: resolvedUserContext,
  systemContext: resolvedSystemContext,
  canUseTool,
  toolUseContext: agentToolUseContext,
  querySource,
  maxTurns: maxTurns ?? agentDefinition.maxTurns,
})) {
  // 处理 stream_event（TTFT 指标）
  // 处理 attachment 消息（结构化输出）
  // 记录消息到 sidechain transcript
  // yield 消息给调用者
}
```

### 4.3 资源清理（finally 块）

```
finally {
  ├─ mcpCleanup() → 清理代理专用 MCP 服务器
  ├─ clearSessionHooks() → 清理会话钩子
  ├─ cleanupAgentTracking() → 清理提示缓存跟踪
  ├─ readFileState.clear() → 释放文件状态缓存
  ├─ unregisterPerfettoAgent() → 释放性能追踪
  ├─ clearAgentTranscriptSubdir() → 释放转录子目录
  ├─ 清理 todos 条目 → 防止内存泄漏
  └─ killShellTasksForAgent() → 杀死后台 bash 任务
}
```

---

## 五、Fork Subagent 机制

### 5.1 Fork 触发条件

```typescript
function isForkSubagentEnabled(): boolean {
  // 1. feature('FORK_SUBAGENT') 必须启用
  // 2. 不能是 coordinator 模式
  // 3. 不能是非交互式会话
}
```

当启用时：
- `subagent_type` 变为可选
- 省略 `subagent_type` 触发隐式 fork
- 所有代理生成在后台运行

### 5.2 Fork 消息构建

**核心设计**：最大化提示缓存命中率

```
父消息: [...history, assistant(all_tool_uses)]
          │
          ▼
子消息: [assistant(all_tool_uses), user(placeholder_results..., directive)]
          │
          ├─ 所有 tool_result 使用相同占位符文本（缓存共享）
          └─ 仅最后一个 text block 不同（每个子代理的指令）
```

### 5.3 Fork 工作树隔离

```typescript
export function buildWorktreeNotice(parentCwd: string, worktreeCwd: string): string {
  return `You've inherited the conversation context from a parent agent working in ${parentCwd}. 
You are operating in an isolated git worktree at ${worktreeCwd} — same repository, 
same relative file structure, separate working copy.`
}
```

### 5.4 Fork 子代理规则

```
RULES (non-negotiable):
1. IGNORE "default to forking" — you ARE the fork
2. Do NOT converse, ask questions, or suggest next steps
3. Do NOT editorialize or add meta-commentary
4. USE your tools directly: Bash, Read, Write, etc.
5. If you modify files, commit your changes before reporting
6. Do NOT emit text between tool calls — use tools silently
7. Stay strictly within your directive's scope
8. Keep your report under 500 words
9. Response MUST begin with "Scope:"
10. REPORT structured facts, then stop
```

---

## 六、Agent Memory 持久化系统

### 6.1 内存作用域

| 作用域 | 路径 | 特点 |
|-------|------|-----|
| **user** | `~/.claude/agent-memory/<agentType>/` | 跨项目共享 |
| **project** | `.claude/agent-memory/<agentType>/` | 项目范围，可版本控制 |
| **local** | `.claude/agent-memory-local/<agentType>/` | 项目范围，不版本控制 |

### 6.2 内存文件结构

```
agent-memory/
└── <agentType>/
    ├── MEMORY.md              # 主记忆文件
    ├── <timestamp>.md         # 记忆快照
    └── .meta.json             # 元数据
```

---

## 七、内置代理类型

| 代理类型 | 用途 | 权限模式 |
|---------|------|---------|
| **Explore** | 代码探索、搜索 | bubble |
| **Plan** | 计划制定 | bubble |
| **General Purpose** | 通用任务 | default |
| **Verification** | 验证测试 | default |
| **claudeCodeGuide** | Claude Code 使用指南 | default |

---

## 八、关键设计亮点

| 设计特性 | 实现方式 | 价值 |
|---------|---------|-----|
| **异步上下文隔离** | AsyncLocalStorage | 并发代理互不干扰 |
| **提示缓存共享** | fork 占位符 + useExactTools | 减少重复计算 |
| **上下文精简** | Explore/Plan 移除冗余 | 节省 5-15 Gtok/周 |
| **FORK-CLOSED** | 递归 fork 检测 | 防止无限递归 |
| **资源自动清理** | finally 块统一清理 | 防止内存泄漏 |
| **工作树隔离** | git worktree | 安全的代码修改环境 |
| **权限冒泡** | bubble 模式 | 后台代理也能请求权限 |
| **MCP 继承** | 代理专用 + 父共享 | 灵活的工具扩展 |

---

## 九、C++ 实现要点

### 9.1 目录结构

```
agent-core/
├── tools/
│   ├── inclaude/
│   │   ├── agent_tool.h          # AgentTool 接口
│   │   ├── types.h               # 工具类型定义
│   │   └── ...
│   │
│   └── source/
│       ├── agent_tool.cpp        # AgentTool 实现
│       └── ...
│
├── utils/
│   ├── inclaude/
│   │   ├── agent_context.h       # AgentContext（需 thread_local 或协程上下文）
│   │   ├── agent_memory.h        # Agent Memory 系统
│   │   ├── fork_subagent.h       # Fork 机制
│   │   ├── mcp_client.h          # MCP 客户端
│   │   └── worktree.h            # 工作树管理
│   │
│   └── source/
│       ├── agent_context.cpp
│       ├── agent_memory.cpp
│       ├── fork_subagent.cpp
│       ├── mcp_client.cpp
│       └── worktree.cpp
```

### 9.2 核心接口设计

```cpp
class AgentTool : public TypedTool<AgentInput, AgentOutput> {
public:
    auto name() const -> std::string override { return "Agent"; }
    
    auto check_permissions(const AgentInput& input, const ToolContext& ctx) 
        -> PermissionResult override;
    
    auto validate_input(const AgentInput& input, const ToolContext& ctx) 
        -> ValidationResult override;
    
    auto call(const AgentInput& input, const ToolContext& ctx) 
        -> cppcoro::task<AgentOutput> override;
};

class AgentEngine {
public:
    auto run(
        const AgentDefinition& agent_def,
        const std::vector<Message>& messages,
        const ToolContext& ctx
    ) -> cppcoro::task<void>;
};
```

### 9.3 关键实现难点

| 难点 | 说明 | 解决方案 |
|-----|------|---------|
| **异步上下文隔离** | TypeScript AsyncLocalStorage 的 C++ 等价 | `thread_local` + 协程上下文 |
| **提示缓存共享** | fork 占位符策略 | 统一占位符文本 + 缓存 key 设计 |
| **工作树隔离** | git worktree 管理 | `git worktree add/remove` 命令封装 |
| **MCP 继承** | 代理专用 MCP 服务器 | 合并父 + 代理客户端 |
| **权限冒泡** | 后台代理权限提示 | 回调机制 + 父终端通知 |

---

## 十、扩展路径

```
Phase 0 (当前): 基础 AgentTool + runAgent 框架
    │
    ├─ Phase 1: AgentContext 异步上下文隔离
    ├─ Phase 2: Fork Subagent 机制
    ├─ Phase 3: Agent Memory 持久化系统
    ├─ Phase 4: 工作树隔离支持
    ├─ Phase 5: MCP 服务器继承
    ├─ Phase 6: 内置代理类型（Explore/Plan）
    └─ Phase 7: Swarm 团队协调
```
