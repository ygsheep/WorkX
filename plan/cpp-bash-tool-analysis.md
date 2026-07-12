# Claude Code BashTool 架构分析

> C++ Agent BashTool 实现参考文档
> 目录结构：`agent-core/tools/inclaude/*.h` + `agent-core/utils/inclaude/*.h`（头文件）
>          `agent-core/tools/source/*.cpp` + `agent-core/utils/source/*.cpp`（源文件）

---

## 一、整体架构概览

BashTool 是 Claude Code 中最复杂的工具之一，采用**多层安全防护**设计：

```
用户输入命令
    │
    ▼
┌──────────────────────────────────────────────────────────────┐
│                    BashTool 执行管道                          │
├──────────────────────────────────────────────────────────────┤
│  1. 命令分类检测                                             │
│     ├─ isSearchOrReadBashCommand() → 搜索/读取/列表命令      │
│     └─ isSilentBashCommand() → 静默命令检测                   │
│                        │                                     │
│                        ▼                                     │
│  2. 权限检查管道                                             │
│     ├─ bashToolCheckExactMatchPermission() → 精确匹配规则    │
│     ├─ bashToolCheckPermission() → 前缀/通配符规则           │
│     ├─ checkPathConstraints() → 路径约束检查                 │
│     ├─ checkSedConstraints() → sed 约束检查                  │
│     ├─ checkPermissionMode() → 模式权限检查                  │
│     └─ checkReadOnlyConstraints() → 只读命令检查             │
│                        │                                     │
│                        ▼                                     │
│  3. 安全分析                                                 │
│     ├─ parseForSecurity() → AST 解析 (tree-sitter)          │
│     ├─ stripSafeWrappers() → 安全包装器剥离                  │
│     └─ stripAllLeadingEnvVars() → 环境变量剥离               │
│                        │                                     │
│                        ▼                                     │
│  4. 沙箱决策                                                 │
│     └─ shouldUseSandbox() → 是否启用沙箱                     │
│                        │                                     │
│                        ▼                                     │
│  5. 命令执行                                                 │
│     └─ spawnShellTask() → 本地 shell 任务执行                │
│                        │                                     │
│                        ▼                                     │
│  6. 结果处理                                                 │
│     ├─ interpretCommandResult() → 语义解释                   │
│     ├─ buildImageToolResult() → 图片输出处理                 │
│     └─ renderToolResultMessage() → UI 渲染                   │
└──────────────────────────────────────────────────────────────┘
```

---

## 二、命令分类系统

### 2.1 命令类型定义

| 命令类型 | 包含命令 | 用途 |
|---------|---------|------|
| **搜索命令** | `find`, `grep`, `rg`, `ag`, `ack`, `locate`, `which`, `whereis` | 内容搜索 |
| **读取命令** | `cat`, `head`, `tail`, `less`, `more`, `wc`, `stat`, `file`, `strings`, `jq`, `awk`, `cut`, `sort`, `uniq`, `tr` | 文件读取和数据处理 |
| **列表命令** | `ls`, `tree`, `du` | 目录列表 |
| **静默命令** | `mv`, `cp`, `rm`, `mkdir`, `rmdir`, `chmod`, `chown`, `chgrp`, `touch`, `ln`, `cd`, `export`, `unset`, `wait` | 无标准输出的命令 |
| **语义中性命令** | `echo`, `printf`, `true`, `false`, `:` | 纯输出/状态命令，不影响管道性质 |

### 2.2 分类逻辑

```typescript
function isSearchOrReadBashCommand(command: string): {
  isSearch: boolean;
  isRead: boolean;
  isList: boolean;
}
```

**关键设计**：对于管道命令（如 `cat file | grep pattern`），**所有部分必须都是搜索/读取命令**，整个命令才被视为可折叠的只读操作。

---

## 三、权限检查管道

### 3.1 权限规则类型

| 规则类型 | 匹配方式 | 示例 |
|---------|---------|------|
| **exact** | 精确匹配 | `"npm run test"` |
| **prefix** | 前缀匹配（带词边界） | `"npm run:*"` |
| **wildcard** | 通配符匹配 | `"git *"` |

### 3.2 检查顺序

```
1. 精确匹配拒绝规则 → deny
2. 精确匹配询问规则 → ask
3. 精确匹配允许规则 → allow
4. 前缀/通配符拒绝规则 → deny
5. 前缀/通配符询问规则 → ask
6. 路径约束检查 → deny/ask/passthrough
7. sed 约束检查 → deny/ask/passthrough
8. 模式权限检查 → allow/passthrough
9. 只读命令检查 → allow
10. 无匹配 → passthrough（触发权限提示）
```

### 3.3 安全防护机制

#### 环境变量处理

| 处理方式 | 适用规则 | 说明 |
|---------|---------|------|
| `stripSafeWrappers()` | allow 规则 | 仅剥离安全的环境变量（NODE_ENV, GOOS 等） |
| `stripAllLeadingEnvVars()` | deny/ask 规则 | 剥离所有环境变量，防止绕过拒绝规则 |

**安全白名单**：

```typescript
const SAFE_ENV_VARS = new Set([
  // Go 构建设置
  'GOEXPERIMENT', 'GOOS', 'GOARCH', 'CGO_ENABLED', 'GO111MODULE',
  // Rust 日志设置
  'RUST_BACKTRACE', 'RUST_LOG',
  // Node 环境
  'NODE_ENV',
  // Python 设置
  'PYTHONUNBUFFERED', 'PYTHONDONTWRITEBYTECODE',
  // 认证密钥
  'ANTHROPIC_API_KEY',
  // 语言环境
  'LANG', 'LC_ALL', 'LC_CTYPE',
  // 终端设置
  'TERM', 'COLORTERM', 'NO_COLOR', 'TZ',
]);
```

**危险环境变量黑名单**：

```typescript
const BINARY_HIJACK_VARS = /^(LD_|DYLD_|PATH$)/
```

#### 包装器命令处理

```typescript
const SAFE_WRAPPER_PATTERNS = [
  /^timeout[ \t]+(...)/,      // timeout 命令
  /^time[ \t]+(?:--[ \t]+)?/, // time 命令
  /^nice(?:[ \t]+-n[ \t]+-?\d+|[ \t]+-\d+)?[ \t]+(?:--[ \t]+)?/, // nice 命令
  /^stdbuf(?:[ \t]+-[ioe][LN0-9]+)+[ \t]+(?:--[ \t]+)?/, // stdbuf 命令
  /^nohup[ \t]+(?:--[ \t]+)?/, // nohup 命令
];
```

#### 复合命令安全

**关键安全规则**：前缀规则（如 `Bash(cd:*)`）**不得匹配复合命令**（如 `"cd /path && python3 evil.py"`）。

---

## 四、AST 安全解析

### 4.1 设计原则：FAIL-CLOSED

使用 tree-sitter-bash 解析命令，采用**显式白名单**方式：

- 只处理理解的节点类型
- 遇到未知节点类型 → 标记为 `too-complex` → 触发权限提示

### 4.2 解析结果类型

```typescript
type ParseForSecurityResult =
  | { kind: 'simple'; commands: SimpleCommand[] }
  | { kind: 'too-complex'; reason: string; nodeType?: string }
  | { kind: 'parse-unavailable' }
```

### 4.3 SimpleCommand 结构

```typescript
type SimpleCommand = {
  argv: string[]                          // 命令参数（引号已解析）
  envVars: { name: string; value: string }[] // 前置环境变量赋值
  redirects: Redirect[]                   // 重定向列表
  text: string                            // 原始命令文本
}
```

### 4.4 支持的节点类型

| 类型 | 说明 |
|-----|------|
| `program` | 根节点 |
| `list` | 复合命令列表（`a && b || c`） |
| `pipeline` | 管道命令（`a \| b`） |
| `redirected_statement` | 带重定向的命令 |
| `&&`, `\|\|`, `\|`, `;`, `&` | 命令分隔符 |

---

## 五、沙箱系统

### 5.1 沙箱决策流程

```typescript
function shouldUseSandbox(input): boolean
```

**决策逻辑**：

```
SandboxManager.isSandboxingEnabled() → false → 不启用沙箱
    │
    ▼
dangerouslyDisableSandbox && areUnsandboxedCommandsAllowed() → true → 不启用沙箱
    │
    ▼
command 为空 → 不启用沙箱
    │
    ▼
containsExcludedCommand(command) → true → 不启用沙箱
    │
    ▼
启用沙箱
```

### 5.2 排除命令匹配

支持三种模式：

| 模式 | 示例 | 说明 |
|-----|------|------|
| **exact** | `"npm run lint"` | 精确匹配 |
| **prefix** | `"npm run test:*"` | 前缀匹配 |
| **wildcard** | `"bazel *"` | 通配符匹配 |

---

## 六、输入/输出 Schema

### 6.1 输入 Schema

```typescript
{
  command: string,                  // 要执行的命令
  timeout?: number,                 // 超时时间（毫秒）
  description?: string,             // 命令描述
  run_in_background?: boolean,      // 后台运行
  dangerouslyDisableSandbox?: boolean, // 危险操作：禁用沙箱
  _simulatedSedEdit?: {             // 内部：模拟 sed 编辑
    filePath: string,
    newContent: string
  }
}
```

### 6.2 输出 Schema

```typescript
{
  stdout: string,                   // 标准输出
  stderr: string,                   // 标准错误
  rawOutputPath?: string,           // 原始输出文件路径（大输出）
  interrupted: boolean,             // 是否被中断
  isImage?: boolean,                // 是否为图片输出
  backgroundTaskId?: string,        // 后台任务 ID
  backgroundedByUser?: boolean,     // 用户手动后台
  assistantAutoBackgrounded?: boolean, // 助手自动后台
  dangerouslyDisableSandbox?: boolean, // 是否禁用了沙箱
  returnCodeInterpretation?: string, // 退出码语义解释
  noOutputExpected?: boolean,       // 是否预期无输出
  structuredContent?: any[],        // 结构化内容块
  persistedOutputPath?: string,     // 持久化输出路径
  persistedOutputSize?: number      // 输出大小
}
```

---

## 七、关键安全特性

### 7.1 命令注入检测

通过 `bashCommandIsSafeAsync()` 检测潜在的命令注入模式，包括：

- 反斜杠转义的操作符
- 隐藏的命令替换
- 危险的重定向

### 7.2 sed 编辑安全

专门的 `checkSedConstraints()` 检查危险的 sed 操作：

- 阻止危险的 sed 编辑模式
- 通过预览机制确保用户确认

### 7.3 路径约束

`checkPathConstraints()` 检查：

- 路径是否在允许的目录范围内
- 是否涉及设备文件
- 是否涉及 UNC 路径（Windows）
- 是否涉及符号链接攻击

### 7.4 只读命令自动允许

`isReadOnly()` 检查命令是否为只读操作，只读命令自动允许执行，无需用户确认。

---

## 八、执行机制

### 8.1 任务管理

通过 `LocalShellTask` 管理命令执行：

- **前台任务**：阻塞等待完成
- **后台任务**：异步执行，完成时通知用户
- **自动后台**：长时间运行的命令自动转为后台

### 8.2 超时处理

| 超时类型 | 默认值 | 最大值 |
|---------|-------|-------|
| 默认超时 | 30 秒 | 10 分钟 |

### 8.3 输出处理

- 大输出（>30K 字符）→ 持久化到文件
- 图片输出 → 自动检测并渲染
- 搜索命令输出 → 可折叠显示

---

## 九、设计亮点总结

| 设计特性 | 实现方式 | 安全价值 |
|---------|---------|---------|
| **FAIL-CLOSED** | AST 白名单解析 | 未知命令自动触发权限提示 |
| **多层防护** | 规则 + 路径 + sed + 模式检查 | 纵深防御 |
| **环境变量隔离** | 安全白名单 + 危险黑名单 | 防止环境变量注入 |
| **复合命令安全** | 前缀规则不匹配复合命令 | 防止绕过拒绝规则 |
| **沙箱机制** | 可配置的文件系统/网络限制 | 限制命令影响范围 |
| **只读自动允许** | 命令分类 + 只读检查 | 提升用户体验 |
| **输出大小限制** | 30K 字符阈值 | 防止资源耗尽 |
| **预览机制** | sed 编辑预览 | 确保用户确认危险操作 |

---

## 十、C++ 实现要点

### 10.1 目录结构

```
agent-core/
├── tools/
│   ├── inclaude/
│   │   ├── bash_tool.h          # BashTool 接口
│   │   └── types.h              # 工具类型定义
│   │
│   └── source/
│       └── bash_tool.cpp        # BashTool 实现
│
├── utils/
│   ├── inclaude/
│   │   ├── bash_ast.h           # AST 解析（需 tree-sitter 绑定）
│   │   ├── bash_permissions.h   # 权限检查逻辑
│   │   ├── shell_exec.h         # 命令执行封装
│   │   └── sandbox.h            # 沙箱接口
│   │
│   └── source/
│       ├── bash_ast.cpp
│       ├── bash_permissions.cpp
│       ├── shell_exec.cpp
│       └── sandbox.cpp
```

### 10.2 核心接口设计

```cpp
class BashTool : public TypedTool<BashInput, BashOutput> {
public:
    auto name() const -> std::string override { return "Bash"; }
    
    auto check_permissions(const BashInput& input, const ToolContext& ctx) 
        -> PermissionResult override;
    
    auto validate_input(const BashInput& input, const ToolContext& ctx) 
        -> ValidationResult override;
    
    auto call(const BashInput& input, const ToolContext& ctx) 
        -> cppcoro::task<BashOutput> override;
};
```

### 10.3 关键实现难点

| 难点 | 说明 | 解决方案 |
|-----|------|---------|
| **tree-sitter-bash 绑定** | 需要 C++ 版本的 AST 解析 | 使用 tree-sitter C API + 手动绑定 |
| **权限规则引擎** | 实现精确/前缀/通配符三种匹配模式 | 正则表达式 + 词边界检查 |
| **命令分类** | 实现搜索/读取/列表/静默命令的分类逻辑 | 哈希表 + 管道解析 |
| **沙箱集成** | 需要操作系统级别的文件系统/网络限制 | Linux: seccomp / Windows: Job Object |
| **异步执行** | 支持后台任务和超时 | std::async + std::future + 信号处理 |

---

## 十一、扩展路径

```
Phase 0 (当前): 基础命令执行 + 权限检查框架
    │
    ├─ Phase 1: 命令分类系统（搜索/读取/列表/静默）
    ├─ Phase 2: AST 安全解析（tree-sitter 绑定）
    ├─ Phase 3: 沙箱集成（文件系统/网络限制）
    ├─ Phase 4: 后台任务管理（异步执行 + 通知）
    ├─ Phase 5: sed 编辑预览机制
    └─ Phase 6: 输出处理（图片检测、大输出持久化）
```
