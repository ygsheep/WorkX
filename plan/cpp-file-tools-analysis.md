# Claude Code File Tools 架构分析

> C++ Agent 文件工具实现参考文档
> 目录结构：`agent-core/tools/inclaude/*.h` + `agent-core/utils/inclaude/*.h`（头文件）
>          `agent-core/tools/source/*.cpp` + `agent-core/utils/source/*.cpp`（源文件）

---

## 一、整体架构概览

Claude Code 的文件操作采用**双通道设计**：

```
用户输入 "@main.cpp"
    │
    ├── 通道1: @mention 直接注入 ──→ 提取文件内容 → AttachmentMessage → 与用户消息一起发送给LLM
    │
    └── 通道2: LLM工具调用 ──→ LLM决定使用Read/Edit/Write工具 → 工具执行管道 → 返回结果
```

**设计原则**：`@filename` 不是单独的输入类型，而是从文本中提取的附件。路由类型（SlashCommand/Bash/Text）与附件是正交的，可以共存。

---

## 二、目录结构

```
agent-core/
├── tools/                         # 工具模块（FileReadTool、FileEditTool 等）
│   ├── inclaude/                  # 头文件目录
│   │   ├── tool.h                 # Tool 基类
│   │   ├── file_read_tool.h       # FileReadTool 接口
│   │   ├── file_edit_tool.h       # FileEditTool 接口
│   │   ├── file_write_tool.h      # FileWriteTool 接口
│   │   ├── glob_tool.h            # GlobTool 接口
│   │   ├── grep_tool.h            # GrepTool 接口
│   │   ├── registry.h             # 工具注册表
│   │   ├── executor.h             # 工具执行器
│   │   └── types.h                # 工具相关类型定义
│   │
│   └── source/                    # 源文件目录
│       ├── tool.cpp               # Tool 基类实现
│       ├── file_read_tool.cpp     # FileReadTool 实现
│       ├── file_edit_tool.cpp     # FileEditTool 实现
│       ├── file_write_tool.cpp    # FileWriteTool 实现
│       ├── glob_tool.cpp          # GlobTool 实现
│       ├── grep_tool.cpp          # GrepTool 实现
│       ├── registry.cpp           # 注册表实现
│       └── executor.cpp           # 执行器实现
│
├── utils/                         # 工具函数模块（文件操作、路径处理等）
│   ├── inclaude/                  # 头文件目录
│   │   ├── file.h                 # 文件操作工具
│   │   ├── path.h                 # 路径处理工具
│   │   ├── diff.h                 # 差异计算工具
│   │   └── permissions.h          # 权限检查工具
│   │
│   └── source/                    # 源文件目录
│       ├── file.cpp               # 文件操作工具实现
│       ├── path.cpp               # 路径处理工具实现
│       ├── diff.cpp               # 差异计算工具实现
│       └── permissions.cpp        # 权限检查工具实现
│
└── CMakeLists.txt                 # CMake 构建文件
```

---

## 三、完整管道图（文件增删改查）

```
用户输入 "@main.cpp"
    │
    ├─────────────────────────────────────────────────────────────────┐
    │  通道1：@mention 直接注入                                        │
    │  ┌─────────────────────────────────────────────────────────┐   │
    │  │ extractAtMentionedFiles() → 正则匹配 @filename         │   │
    │  │ parseAtMentionedFileLines() → 解析行范围 @file#L10-20   │   │
    │  │ processAtMentionedFiles() → 权限检查 + 文件读取         │   │
    │  │ generateFileAttachment() → 生成 AttachmentMessage      │   │
    │  └─────────────────────────────────────────────────────────┘   │
    │                           │                                    │
    │                           ▼                                    │
    │                 注入 UserMessage                                │
    └───────────────────────────┼────────────────────────────────────┘
                                ▼
                           发送给 LLM
                                │
                                │  LLM 决定调用工具
                                ▼
    ┌─────────────────────────────────────────────────────────────────┐
    │  通道2：LLM 工具调用（ReAct 循环）                                │
    │                                                                 │
    │  Read (读)                    Edit (改)                    Write (增/覆盖)
    │  ┌──────────────┐            ┌──────────────┐            ┌──────────────┐
    │  │checkPermissions│           │checkPermissions│           │checkPermissions│
    │  │validateInput   │           │validateInput   │           │validateInput   │
    │  │call            │           │call            │           │call            │
    │  │  ├─ 去重检查   │           │  ├─ 编码检测   │           │  ├─ 存在检查   │
    │  │  ├─ 格式检测   │           │  ├─ 字符串匹配  │           │  ├─ 内容写入   │
    │  │  └─ 内容读取   │           │  └─ 内容替换   │           │  └─ 生成Diff   │
    │  │mapToolResult   │           │mapToolResult   │           │mapToolResult   │
    │  └──────────────┘            └──────────────┘            └──────────────┘
    │                                                                 │
    │  Delete (删) — 通过 BashTool 执行: rm /path/to/file             │
    │                                                                 │
    └─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
                          返回工具结果
                                │
                                ▼
                          更新对话历史
                                │
                                ▼
                          LLM 继续思考
```

---

## 四、通道1：@mention 直接注入流程

当用户输入 `"帮我阅读 @main.cpp"` 时，文件内容在发送给LLM之前就被提取并注入为附件。

### 4.1 管道图

```
用户输入: "帮我阅读 @main.cpp"
    │
    ▼
extractAtMentionedFiles()  [attachments.ts:2757]
    │  ├─ 正则匹配: @"path with spaces" 和 @filename
    │  ├─ 排除代理引用: @"agent-type (agent)"
    │  └─ 去重返回文件路径列表
    ▼
parseAtMentionedFileLines()  [attachments.ts:2836]
    │  ├─ 解析行范围: @file#L10-20
    │  ├─ 解析单个行: @file#L10
    │  └─ 解析锚点: @file#heading
    ▼
processAtMentionedFiles()  [attachments.ts:1894]
    │  ├─ 权限检查: isFileReadDenied()
    │  ├─ 目录检测: stat() → 如果是目录则列出内容
    │  └─ generateFileAttachment() → 读取文件内容
    ▼
AttachmentMessage 注入
    │
    ▼
与 UserMessage 一起发送给 LLM
```

### 4.2 与通道2的连接

**关键设计**：@mention路径内部复用FileReadTool读取文件内容，而非重新实现文件读取逻辑。

```
InputProcessor::process_text_prompt()
    │
    ├─ 提取 @引用 → FileMention 列表
    ├─ 遍历 FileMention:
    │   └─ FileReadTool::read_text_file(path, line_start, line_end)
    │       └─ 返回文件内容片段
    ├─ 组装消息列表:
    │   ├─ 用户文本
    │   └─ [File: xxx] + 文件内容
    └─ 返回 ProcessResult{should_query: true, messages: [...]}
```

这样两个通道共享同一套文件读取基础设施，保证行为一致性。

---

## 五、通道2：LLM 工具调用流程

当 LLM 需要读取/编辑文件时，会主动调用文件工具。工具执行有统一的管道。

### 5.1 工具执行管道

```
LLM 决定调用工具: {"name": "Read", "input": {"file_path": "/path/to/file"}}
    │
    ▼
runToolUse()  [toolExecution.ts:337]
    │  ├─ findToolByName() → 查找工具定义
    │  ├─ 检查工具是否存在
    │  └─ 检查是否被取消
    ▼
streamedCheckPermissionsAndCallTool()  [toolExecution.ts:492]
    │  ├─ runPreToolUseHooks() → 前置钩子
    │  ├─ checkPermissions() → 权限检查
    │  ├─ validateInput() → 输入验证
    │  ├─ call() → 核心执行
    │  ├─ runPostToolUseHooks() → 后置钩子
    │  └─ mapToolResultToToolResultBlockParam() → 结果映射
    ▼
返回 ToolResultBlockParam
    │
    ▼
注入对话历史 → LLM 继续思考
```

---

## 六、文件工具详细分析

### 6.1 FileReadTool — 文件读取

| 层级 | 方法 | 职责 |
|-----|------|-----|
| **定义层** | `buildTool({ name: 'Read', ... })` | 工具元数据、Schema、提示词 |
| **权限层** | `checkPermissions()` | 检查文件读取权限 |
| **验证层** | `validateInput()` | 路径验证、二进制检查、设备文件拦截 |
| **执行层** | `call()` | 核心读取逻辑、去重优化、多格式处理 |
| **结果层** | `mapToolResultToToolResultBlockParam()` | 结果格式转换 |

**支持的文件格式**：
- `text` — 普通文本文件（支持行号、偏移量）
- `image` — 图片文件（PNG/JPG等，Base64编码）
- `notebook` — Jupyter Notebook（.ipynb）
- `pdf` — PDF文件（支持分页读取）
- `file_unchanged` — 去重返回的stub

**去重优化**：如果文件未修改且读取过相同范围，返回stub减少Token消耗。

### 6.2 FileEditTool — 文件编辑

| 层级 | 方法 | 职责 |
|-----|------|-----|
| **定义层** | `buildTool({ name: 'Edit', ... })` | 工具元数据、Schema |
| **权限层** | `checkPermissions()` | 检查文件写入权限 |
| **验证层** | `validateInput()` | 编码检测、文件大小限制、字符串匹配验证 |
| **执行层** | `call()` | 内容替换、文件写入、差异计算 |
| **结果层** | `mapToolResultToToolResultBlockParam()` | 返回diff和原始内容 |

**编辑模式**：
- `replace_all: false` — 替换第一个匹配
- `replace_all: true` — 替换所有匹配

### 6.3 FileWriteTool — 文件写入

| 层级 | 方法 | 职责 |
|-----|------|-----|
| **定义层** | `buildTool({ name: 'Write', ... })` | 工具元数据、Schema |
| **权限层** | `checkPermissions()` | 检查文件写入权限 |
| **验证层** | `validateInput()` | 路径验证、安全检查 |
| **执行层** | `call()` | 文件写入、差异计算、Git diff |
| **结果层** | 返回 `create` 或 `update` 类型 |

---

## 七、关键设计要点

### 7.1 权限系统

所有文件操作都经过权限检查：

```
checkReadPermissionForTool(FileReadTool, input, context)
checkWritePermissionForTool(FileEditTool, input, context)
```

权限规则支持：
- `deny` — 拒绝访问
- `allow` — 允许访问
- 通配符匹配（`**/*.md`）

### 7.2 安全防护

| 防护项 | 说明 |
|-------|------|
| **设备文件拦截** | `/dev/zero`、`/dev/stdin` 等危险路径 |
| **UNC路径保护** | 防止Windows NTLM凭证泄露 |
| **二进制文件检查** | 拒绝读取二进制文件（除图片/PDF） |
| **团队内存密钥检查** | 编辑时检测敏感信息 |

### 7.3 编码自动检测

```
UTF-16LE 检测（BOM: FF FE）→ 'utf16le'
其他 → 'utf8'
```

### 7.4 去重优化（FileReadTool）

```
readFileState 缓存:
    ├─ 记录文件路径 + 读取范围 + 最后修改时间
    └─ 调用时检查：若mtime未变化，返回 file_unchanged stub
```

减少约 **18%** 的重复读取，显著节省 Token。

---

## 八、C++ 核心接口设计

### 8.1 ITool 接口基类

```cpp
/**
 * @file itool.h
 * @brief ITool 接口 — 工具抽象基类
 * @details 所有 Agent 可调用工具的统一接口：名称、描述、参数 schema、权限检查、输入验证、执行
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include "core/utils/result.h"
#include "agent/tool/result.h"
#include "agent/tool/context.h"

namespace agent::tool {

/// 权限检查结果类型
using PermissionResult = Result<void, std::string>;

/// 输入验证结果类型
using ValidationResult = Result<void, std::string>;

/// @brief ITool 接口 — 工具抽象基类
///
/// 所有 Agent 可调用工具的统一接口：
/// - 名称、描述、参数 schema 声明
/// - 权限检查、输入验证、执行
class ITool {
public:
    virtual ~ITool() = default;

    /// @brief 工具名称
    /// @return 工具名称字符串
    virtual const std::string& name() const = 0;

    /// @brief 工具描述
    /// @return 工具描述字符串
    virtual const std::string& description() const = 0;

    /// @brief 工具提示词
    /// @return 提示词字符串
    virtual const std::string& prompt() const = 0;

    /// @brief 检查工具调用权限
    /// @param input 工具输入参数
    /// @param ctx 工具执行上下文
    /// @return 权限检查结果
    virtual PermissionResult check_permissions(
        const nlohmann::json& input,
        const ToolContext& ctx
    ) const;

    /// @brief 验证工具输入
    /// @param input 工具输入参数
    /// @param ctx 工具执行上下文
    /// @return 验证结果
    virtual ValidationResult validate_input(
        const nlohmann::json& input,
        const ToolContext& ctx
    ) const;

    /// @brief 执行工具
    /// @param input 工具输入参数
    /// @param ctx 工具执行上下文
    /// @return 工具执行结果
    virtual ToolResult call(
        const nlohmann::json& input,
        const ToolContext& ctx
    ) = 0;
};

} // namespace agent::tool
```

### 8.2 TypedTool 模板

```cpp
/**
 * @file typed_tool.h
 * @brief TypedTool 模板 — 强类型工具基类
 * @details 提供输入/输出类型安全的工具接口，自动处理 JSON 与类型转换
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <nlohmann/json.hpp>
#include "itool.h"

namespace agent::tool {

/// @brief TypedTool 模板 — 强类型工具基类
///
/// @tparam Input 工具输入参数类型（需可从 nlohmann::json 转换）
/// @tparam Output 工具输出结果类型（需可转换为 nlohmann::json）
///
/// 子类实现 call(const Input&, const ToolContext&) 即可，
/// 基类负责 JSON ↔ Input/Output 的转换与错误处理。
template<typename Input, typename Output>
class TypedTool : public ITool {
public:
    /// @brief 执行工具（类型安全版本）
    /// @param input 强类型输入参数
    /// @param ctx 工具执行上下文
    /// @return 工具执行结果
    virtual ToolResult call(
        const Input& input,
        const ToolContext& ctx
    ) = 0;

    /// @brief JSON 输入转换为强类型后调用
    ToolResult call(
        const nlohmann::json& input,
        const ToolContext& ctx
    ) final {
        Input typed_input;
        try {
            typed_input = input.get<Input>();
        } catch (const nlohmann::json::exception& e) {
            return ToolResult::error(std::format("输入参数解析失败: {}", e.what()));
        }
        return call(typed_input, ctx);
    }
};

} // namespace agent::tool
```

### 8.3 工具执行器

```cpp
/**
 * @file executor.h
 * @brief ToolExecutor — 工具执行器
 * @details 负责查找工具、权限检查、输入验证、执行工具并返回结果
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <memory>
#include <nlohmann/json.hpp>
#include "itool.h"
#include "registry.h"
#include "agent/tool/result.h"
#include "agent/tool/context.h"

namespace agent::tool {

/// @brief 工具执行结果
struct ExecutionResult {
    std::string tool_name;              ///< 工具名称
    ToolResult result;                  ///< 工具返回结果
    bool is_error{false};               ///< 是否出错
};

/// @brief ToolExecutor — 工具执行器
///
/// 负责实际执行 LLM 请求的工具调用：
/// - 按名称查找工具
/// - 权限检查、输入验证
/// - 调用工具并返回结果
/// - 使用同步返回类型，不用 cppcoro::task
class ToolExecutor {
public:
    /// @brief 构造函数
    /// @param registry 工具注册表
    explicit ToolExecutor(std::shared_ptr<ToolRegistry> registry);

    /// @brief 执行工具
    /// @param tool_name 工具名称
    /// @param input 工具输入参数
    /// @param ctx 工具执行上下文
    /// @return 执行结果
    ExecutionResult execute(
        const std::string& tool_name,
        const nlohmann::json& input,
        const ToolContext& ctx
    ) const;

private:
    std::shared_ptr<ToolRegistry> m_registry;
};

} // namespace agent::tool
```

---

## 九、工具注册表

```cpp
/**
 * @file registry.h
 * @brief ToolRegistry — 工具注册表
 * @details 管理所有可用工具的生命周期：注册、查找、列举
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include "itool.h"

namespace agent::tool {

/// @brief ToolRegistry — 工具注册表
///
/// 管理所有可用工具的生命周期：
/// - 注册/注销工具实例
/// - 按名称查找工具
/// - 列举所有工具的 schema（供 LLM function calling 使用）
class ToolRegistry {
public:
    /// @brief 注册工具
    /// @param tool 工具实例
    void register_tool(std::shared_ptr<ITool> tool);

    /// @brief 按名称查找工具
    /// @param name 工具名称
    /// @return 工具实例（未找到返回 nullptr）
    std::shared_ptr<ITool> find_by_name(const std::string& name) const;

    /// @brief 获取所有已注册工具
    /// @return 工具列表
    std::vector<std::shared_ptr<ITool>> get_all_tools() const;

    /// @brief 获取工具数量
    /// @return 工具数量
    size_t size() const { return m_tools.size(); }

private:
    std::unordered_map<std::string, std::shared_ptr<ITool>> m_tools;
};

} // namespace agent::tool
```

---

## 十、工具类型定义

| 输入类型 | 字段 |
|---------|------|
| `FileReadInput` | `file_path`, `offset`, `limit`, `pages` |
| `FileEditInput` | `file_path`, `old_string`, `new_string`, `replace_all` |
| `FileWriteInput` | `file_path`, `content` |
| `GlobInput` | `pattern`, `cwd` |
| `GrepInput` | `pattern`, `path`, `case_insensitive`, `regex` |

| 输出类型 | 字段 |
|---------|------|
| `FileReadOutput` | `type`, `file_path`, `content`, `total_lines`, `start_line` |
| `FileEditOutput` | `file_path`, `content`, `structured_patch`, `original_file` |
| `FileWriteOutput` | `type`, `file_path`, `content`, `structured_patch`, `original_file` |
| `GlobOutput` | `files`, `directories` |
| `GrepOutput` | `matches` |

---

## 十一、扩展路径

```
Phase 0 (当前): FileReadTool + FileEditTool + FileWriteTool + GlobTool + GrepTool
    │
    ├─ Phase 1: 权限系统增强（规则引擎）
    ├─ Phase 2: 文件读取去重优化（readFileState 缓存）
    ├─ Phase 3: 多格式支持（PDF/图片/Notebook 解析）
    ├─ Phase 4: 编码自动检测（UTF-8/UTF-16LE）
    └─ Phase 5: 安全防护（设备文件拦截、UNC路径保护）
```
