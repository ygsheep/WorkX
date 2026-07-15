# FileWriteTool — 文件写入工具

> 创建新文件或覆盖现有文件内容，自动创建父目录，更新模式生成行级 diff。
> 基于 Claude Code `FileWriteTool` 源码分析（见附录 A）。
> 版本：v2.1.0（Phase 2 — 安全性增强）

---

## 一、概述

`FileWriteTool` 是 Agent 工具集中负责"写入文件"的工具，对应 LLM 工具名 `Write`。支持：

- **创建新文件**：文件不存在时创建（含父目录自动创建）
- **覆盖现有文件**：文件存在时整体覆盖写入
- **行级 diff 反馈**：更新模式下生成 unified diff 风格文本反馈给 LLM
- **路径解析**：相对路径基于 `ToolContext::cwd` 自动规范化为绝对路径
- **CRLF 兼容**：binary 模式写入，不重写行尾（与 Claude Code 一致）

### Phase 2 安全性增强

- **Pre-read 强制检查**：现有文件必须先 Read 全部，否则拒绝写入（对齐 CC `validateInput`）
- **Staleness 检测**：文件被外部修改（mtime 变化）时拒绝写入；完整读取下若内容未变则放行（避免云同步/杀毒误判）
- **.bak 备份**：覆盖前保存 `<path>.bak`，失败则中止写入（安全优先）
- **写后状态刷新**：写入成功后更新 `FileReadStateTracker`，允许连续写入同一文件

### 文件清单

| 文件 | 用途 | 版本 |
|------|------|------|
| [file_write_tool.h](file_write_tool.h) | 工具接口声明（含 Phase 2 私有方法） | v2.1.0 |
| [file_write_tool.cpp](file_write_tool.cpp) | 工具实现（call 管道 + 安全检查 + schema 对齐） | v2.1.0 |
| [diff.h](diff.h) | 行级 diff 接口（DiffOp / DiffLine / generate_line_diff / format_diff） | v1.0.0 |
| [diff.cpp](diff.cpp) | LCS 算法实现 + unified diff 格式化 | v1.0.0 |
| [../FileReadState/file_read_state.h](../FileReadState/file_read_state.h) | FileReadStateTracker 单例（跨工具读取状态） | v1.0.0 |
| [../FileReadState/file_read_state.cpp](../FileReadState/file_read_state.cpp) | FileReadStateTracker 实现（mutex 保护） | v1.0.0 |

---

## 二、工具元数据

### 2.1 四个核心方法

| 方法 | 返回类型 | 用途 | CC 对齐 |
|------|---------|------|---------|
| `name()` | `const std::string&` | 工具标识 `"Write"` | ✅ 一致 |
| `description()` | `const std::string&` | 简短描述（UI/工具列表） | ✅ 一致 |
| `prompt()` | `const std::string&` | LLM 完整 Usage 说明 | ✅ 一致 |
| `input_schema()` | `nlohmann::json` | 输入参数 JSON Schema | ✅ 一致（`additionalProperties: false`） |

### 2.2 `description()`

```
Write a file to the local filesystem.
```

### 2.3 `prompt()`

```
Writes a file to the local filesystem.

Usage:
- This tool will overwrite the existing file if there is one at the provided path.
- If this is an existing file, you MUST use the Read tool first to read the file's contents.
  This tool will fail if you did not read the file first.
- Prefer the Edit tool for modifying existing files — it only sends the diff.
  Only use this tool to create new files or for complete rewrites.
- NEVER create documentation files (*.md) or README files unless explicitly requested by the User.
- Only use emojis if the user explicitly requests it. Avoid writing emojis to files unless asked.
- The file_path parameter must be an absolute path, not a relative path.
```

> **注**：Phase 2 已通过 `FileReadStateTracker` 强制 pre-read 检查（见第六节）。

### 2.4 `input_schema()`

| 字段 | 类型 | 必填 | 描述 |
|------|------|------|------|
| `file_path` | string | ✅ | 文件绝对路径 |
| `content` | string | ✅ | 写入内容（允许空字符串） |

```json
{
  "type": "object",
  "properties": {
    "file_path": {"type": "string", "description": "The absolute path to the file to write (must be absolute, not relative)"},
    "content": {"type": "string", "description": "The content to write to the file"}
  },
  "required": ["file_path", "content"],
  "additionalProperties": false
}
```

> `additionalProperties: false` 对齐 CC `z.strictObject`，禁止额外字段。

---

## 三、执行管道

```
call(input, ctx)
    │
    ▼
1. 解析 FileWriteInput
    │
    ▼
2. 路径解析（weakly_canonical）
   ├─ 相对路径 → 基于 ctx.cwd 拼接
   └─ weakly_canonical 规范化（无异常重载）
    │
    ▼
3. 创建父目录（create_directories，mkdir -p 语义）
   └─ 失败 → ToolResult::error
    │
    ▼
4. 判断 create/update（fs::exists）
    │
    ├─ 不存在 ──→ 7. 写入文件（std::ofstream binary）
    │                │
    │                ▼
    │           8. 刷新 FileReadStateTracker（写入后状态）
    │                │
    │                ▼
    │           9a. 返回 "File created successfully at: <path>"
    │
    └─ 已存在 ──→ 5. 安全检查（Phase 2）：
                    │
                    ├─ 5a. Pre-read 强制检查
                    │      └─ FileReadStateTracker 无状态 / partial_view → error
                    │
                    ├─ 5b. Staleness 检测
                    │      ├─ current_mtime > state.mtime → 读取当前内容对比
                    │      │   ├─ 内容相同 → 放行（云同步/杀毒误判防护）
                    │      │   └─ 内容不同 → error
                    │      └─ mtime 未变 → 放行
                    │
                    └─ 5c. .bak 备份
                        └─ 失败 → error（安全优先）
                    │
                    ▼
                 6. 读取旧内容（std::ifstream binary，用于 diff）
                    │
                    ▼
                 7. 写入文件（std::ofstream binary）
                    │
                    ▼
                 8. 刷新 FileReadStateTracker（写入后状态）
                    │
                    ▼
                 9b. 生成行级 diff（LCS 算法）
                    │
                    ▼
                 10. 返回 "File <path> has been updated." + diff 文本
```

### 错误处理

所有错误通过 `ToolResult::error()` 返回，不抛异常。所有 `std::filesystem` 操作使用 `std::error_code` 重载。

| 错误场景 | 错误信息 |
|---------|---------|
| 父目录创建失败 | `Failed to create directory '<dir>': <ec.message()>` |
| **Pre-read 未读**（Phase 2） | `File has not been read yet. Read it first before writing to it.` |
| **Pre-read 部分读**（Phase 2） | `File was only partially read. Read the full file before writing to it.` |
| **Staleness 检测失败**（Phase 2） | `File has been modified since read, either by the user or by a linter. Read it again before attempting to write it.` |
| **.bak 备份失败**（Phase 2） | `Failed to create backup '<path>.bak': <ec.message()>` |
| 文件打开失败 | `Failed to open file for writing: <path>` |
| 写入失败（流状态） | `Failed to write file: <path>` |
| 输入解析失败 | `Input parse failed: <json exception>` |

---

## 四、输入验证（validate_input）

| 校验项 | 失败信息 |
|-------|---------|
| `file_path` 字段缺失或非字符串 | `Missing required field: file_path` |
| `file_path` 为空字符串 | `file_path must not be empty` |
| `content` 字段缺失或非字符串 | `Missing required field: content` |

> `content` 允许为空字符串（空文件合法）。`content` 不限制长度（Phase 3 可加入 `MAX_FILE_WRITE_BYTES`）。

---

## 五、Diff 生成（diff.h / diff.cpp）

### 5.1 算法

基于 **LCS（最长公共子序列）** 动态规划算法：

1. 将 `old_content` / `new_content` 按行分割（CRLF 兼容，移除行末 `\r`）
2. 构建 LCS 长度矩阵 `dp[m+1][n+1]`，`dp[i][j]` = `old[0..i)` 与 `new[0..j)` 的 LCS 长度
3. 回溯生成 `DiffLine` 序列：
   - `old[i-1] == new[j-1]` → `Equal`
   - `dp[i-1][j] >= dp[i][j-1]` → `Remove(old[i-1])`
   - 否则 → `Add(new[j-1])`

时间空间复杂度：`O(n*m)`，对中小文件足够（Phase 3 可升级 Myers O(ND)）。

### 5.2 输出格式

unified diff 风格，3 行上下文：

```
--- a/src/main.cpp
+++ b/src/main.cpp
@@ -10,3 +10,4 @@
 int main() {
-    return 0;
+    std::cout << "Hello";
+    return 0;
 }
```

无变更时返回空字符串（`format_diff` 返回 `""`，`call` 返回 `(no content changes)`）。

### 5.3 边界快速路径

| 场景 | 处理 |
|------|------|
| 旧内容为空 | 全部 `Add`，跳过 LCS 矩阵 |
| 新内容为空 | 全部 `Remove`，跳过 LCS 矩阵 |
| 两者都为空 | 返回空 diff |
| 内容相同 | `format_diff` 检测无变更行，返回空字符串 |

---

## 六、Phase 2 安全特性

### 6.1 FileReadStateTracker 单例

跨工具共享的文件读取状态注册表（[../FileReadState/file_read_state.h](../FileReadState/file_read_state.h)）。

**API：**

| 方法 | 调用方 | 用途 |
|------|-------|------|
| `record_read(path, content, mtime, is_partial)` | FileReadTool | 成功读取后记录状态 |
| `get_state(path)` → `optional<FileReadState>` | FileWriteTool | 查询状态做 pre-read/staleness 检查 |
| `update_after_write(path, content, mtime, is_partial)` | FileWriteTool | 写入后刷新状态，允许连续写入 |
| `remove(path)` / `clear()` / `clear_for_test()` | 测试/清理 | 移除状态 |

**FileReadState 字段：**

| 字段 | 类型 | 用途 |
|------|------|------|
| `mtime` | `fs::file_time_type` | 读取时的 mtime，staleness 快速检测 |
| `content` | `std::string` | LF 规范化内容快照，mtime 变化时内容对比回退 |
| `is_partial_view` | `bool` | 是否 offset/limit 部分读取（部分读取不可做内容对比） |

**路径 key 规范化：** 调用方传 `fs::weakly_canonical(path).generic_string()`，保证不同相对路径 / 平台分隔符映射到同一 entry。

**线程安全：** 所有方法通过内部 `std::mutex` 保护。

### 6.2 Pre-read 强制检查

对齐 CC `validateInput` 的 pre-read 逻辑：

| 文件状态 | 检查结果 |
|---------|---------|
| 文件不存在（create 模式） | 跳过检查，允许创建 |
| 文件存在 + 无读取状态 | ❌ 拒绝：`File has not been read yet...` |
| 文件存在 + 状态为 partial_view | ❌ 拒绝：`File was only partially read...` |
| 文件存在 + 状态为完整视图 | ✅ 进入 staleness 检查 |

> CC 要求现有文件必须先 Read 全部，部分读取（offset/limit）不算。本工具对齐此行为。

### 6.3 Staleness 检测

对齐 CC `call()` 中的 staleness 回退逻辑：

```
1. 获取当前文件 mtime（fs::last_write_time）
2. 若 current_mtime <= state.mtime → 放行（mtime 未变）
3. 若 current_mtime > state.mtime（mtime 变化）：
   a. 读取当前文件内容（LF 规范化）
   b. 与 state.content 对比：
      ├─ 相同 → 放行（云同步/杀毒等 mtime 变化但内容未变）
      └─ 不同 → 拒绝：File has been modified since read...
```

**LF 规范化**（`read_file_lf_normalized`）：
1. CRLF (`\r\n`) → LF (`\n`)
2. 孤立 `\r` → LF（旧 Mac 风格）
3. 移除末尾单个 `\n`（对齐 `std::getline` 行为）

### 6.4 .bak 备份

写入前在同目录创建 `<file_path>.bak`：

```cpp
fs::copy_file(file_path, bak_path, fs::copy_options::overwrite_existing, ec);
```

- 备份失败 → 中止写入（安全优先）
- 覆盖已存在的 `.bak`（保留最新版本）
- 不清理旧 `.bak`（由用户/外部工具管理）

### 6.5 写后状态刷新

写入成功后调用 `update_after_write`：

```cpp
FileReadStateTracker::instance().update_after_write(
    file_path.generic_string(),
    lf_normalize(write_input.content),  // LF 规范化新内容
    new_mtime,                          // 写入后的 mtime
    false                               // 完整视图
);
```

- 保证连续写入同一文件通过 staleness 检查
- `lf_normalize` 与 `read_file_lf_normalized` 共用规范化逻辑，保证下次 staleness 内容对比正确

---

## 七、使用示例

### 7.1 创建新文件

**输入：**
```json
{
  "file_path": "/tmp/hello.txt",
  "content": "Hello, World!\n"
}
```

**输出：**
```
File created successfully at: /tmp/hello.txt
```

### 7.2 更新现有文件

**旧内容：**
```
line1
line2
line3
```

**输入：**
```json
{
  "file_path": "/tmp/test.txt",
  "content": "line1\nLINE2\nline3\n"
}
```

**输出：**
```
File /tmp/test.txt has been updated.

--- a//tmp/test.txt
+++ b//tmp/test.txt
@@ -1,3 +1,3 @@
 line1
-line2
+LINE2
 line3
```

### 7.3 自动创建父目录

**输入：**
```json
{
  "file_path": "/tmp/a/b/c/new.txt",
  "content": "nested"
}
```

**输出：**
```
File created successfully at: /tmp/a/b/c/new.txt
```

（`/tmp/a/b/c/` 目录会被自动递归创建）

### 7.4 相对路径解析

**输入：**
```json
{
  "file_path": "src/main.cpp",
  "content": "int main() { return 0; }"
}
```

若 `ctx.cwd = "/home/user/project"`，则实际写入路径为 `/home/user/project/src/main.cpp`。

---

## 八、设计决策

| 决策 | 理由 |
|------|------|
| 同步返回 `ToolResult` | 遵循项目约定，与 FileReadTool 一致 |
| `create_directories` 创建父目录 | `mkdir -p` 语义，幂等 |
| `std::ofstream` binary 模式写入 | 避免平台自动转换行尾；CC 也是直接写入 content |
| LCS 算法生成 diff | 实现简单，O(n*m) 对中小文件足够 |
| Pre-read 强制检查（Phase 2） | 对齐 CC `validateInput`，通过 `FileReadStateTracker` 跟踪读取状态 |
| Staleness mtime + 内容对比回退 | mtime 快速检测，内容对比避免云同步/杀毒误判 |
| .bak 备份失败中止写入 | 安全优先，宁可拒绝写入也不丢失备份 |
| diff 独立为 `diff.h/.cpp` | 便于 FileEditTool 复用 |
| `additionalProperties: false` | 对齐 CC `z.strictObject`，严格 schema |
| `content` 允许空字符串 | 空文件合法 |
| 路径解析复用 FileReadTool 模式 | `weakly_canonical` + `std::error_code` 无异常 |
| 返回文本结果（非结构化 JSON） | 与 FileReadTool 一致，Phase 2 可扩展为结构化 |

---

## 九、与 Claude Code 对比

| 特性 | Claude Code | 本工具 (v2.1.0) | 差异 |
|------|-------------|-----------------|------|
| 文件创建 | ✅ | ✅ | 一致 |
| 文件覆盖 | ✅ | ✅ | 一致 |
| 绝对路径要求 | 必须绝对 | prompt 声明绝对，代码宽容相对 | 本工具更鲁棒 |
| `additionalProperties` | `false` | `false` | 一致 |
| 父目录自动创建 | ✅ `fs.mkdir` | ✅ `create_directories` | 一致 |
| Diff 生成 | `getPatchForDisplay` (Myers) | LCS 行级 diff | 算法不同，均生成 unified diff |
| Pre-read 强制检查 | ✅ `readFileState` | ✅ `FileReadStateTracker` | 一致 |
| Staleness 检查 | ✅ mtime 对比 | ✅ mtime + 内容对比回退 | 本工具更鲁棒（容错云同步） |
| .bak 备份 | ❌ | ✅ `fs::copy_file` | 本工具额外提供 |
| 行尾处理 | 直接写入 content | 直接写入 content | 一致 |
| LSP 集成 | ✅ didChange/didSave | ❌ | 不适用（TUI） |
| 文件历史备份 | ✅ `fileHistoryTrackEdit` | ❌ Phase 3 | 需独立基础设施 |
| Git diff | ✅ `fetchSingleFileGitDiff` | ❌ Phase 3 | 需 git 集成 |
| 返回结构 | 结构化 JSON | 文本结果（含 diff） | 本工具返回文本 |

---

## 十、路线图

### Phase 2 — 安全性增强（已完成 ✅）

- [x] `FileReadStateTracker` 单例（记录已读文件路径 + mtime + 内容快照）
- [x] Pre-read 强制检查（现有文件必须先 Read，部分读取不算）
- [x] Staleness 检查（mtime 变化 + 内容对比回退）
- [x] 文件备份（写入前保存 `.bak`，失败中止写入）
- [x] 写后状态刷新（支持连续写入同一文件）

### Phase 3 — 高级特性（远期）

- [ ] Git diff 集成
- [ ] 文件历史追踪
- [ ] 配置化参数（`MAX_FILE_WRITE_BYTES`）
- [ ] Myers diff 算法（替代 LCS，O(ND) 更高效）
- [ ] UNC 路径安全处理（Windows）

---

## 附录 A：Claude Code 源码分析（参考）

> 原始源码分析文档，保留作为实现参考。源码路径：`claude-code-src/src/tools/FileWriteTool/`。

### A.1 四个核心方法对照

| C++ 方法 | CC 源码对应 | 返回类型 | 用途 |
|---|---|---|---|
| `name()` | `name:` 字段 | `string` | 工具名标识，用于 LLM 调用 |
| `description()` | `async description()` | `string` | 一句话工具描述（简版） |
| `prompt()` | `async prompt()` | `string` | LLM 可见的完整工具说明 |
| `input_schema()` | `get inputSchema` | JSON | 工具输入参数的 JSON Schema |

### A.2 CC 源码 — `name`

[FileWriteTool.ts#L95](file:///d:/develop/Workspace/claude-code-src/src/tools/FileWriteTool/FileWriteTool.ts#L95)：

```typescript
name: FILE_WRITE_TOOL_NAME,  // = 'Write'
```

[prompt.ts#L3](file:///d:/develop/Workspace/claude-code-src/src/tools/FileWriteTool/prompt.ts#L3)：

```typescript
export const FILE_WRITE_TOOL_NAME = 'Write'
```

**作用**：LLM 在 `tool_use` 块中用这个字符串引用工具，如 `{"name": "Write", "input": {...}}`。是工具的唯一标识符。

### A.3 CC 源码 — `description`

[FileWriteTool.ts#L99](file:///d:/develop/Workspace/claude-code-src/src/tools/FileWriteTool/FileWriteTool.ts#L99)：

```typescript
async description() {
  return 'Write a file to the local filesystem.'
}
```

**作用**：返回一句话的简短描述，常用于：
- 工具列表展示（如 `/tools` 命令）
- 工具选择器/分类器
- UI 上的 tooltip

### A.4 CC 源码 — `prompt`

[FileWriteTool.ts#L108](file:///d:/develop/Workspace/claude-code-src/src/tools/FileWriteTool/FileWriteTool.ts#L108)：

```typescript
async prompt() {
  return getWriteToolDescription()
}
```

[prompt.ts#L10-17](file:///d:/develop/Workspace/claude-code-src/src/tools/FileWriteTool/prompt.ts#L10)：

```typescript
export function getWriteToolDescription(): string {
  return `Writes a file to the local filesystem.

Usage:
- This tool will overwrite the existing file if there is one at the provided path.${getPreReadInstruction()}
- Prefer the Edit tool for modifying existing files — it only sends the diff. Only use this tool to create new files or for complete rewrites.
- NEVER create documentation files (*.md) or README files unless explicitly requested by the User.
- Only use emojis if the user explicitly requests it. Avoid writing emojis to files unless asked.`
}
```

`getPreReadInstruction()` 内容（[L6-8](file:///d:/develop/Workspace/claude-code-src/src/tools/FileWriteTool/prompt.ts#L6)）：

```
- If this is an existing file, you MUST use the Read tool first to read the file's contents. This tool will fail if you did not read the file first.
```

### A.5 CC 源码 — `input_schema`

[FileWriteTool.ts#L56-65](file:///d:/develop/Workspace/claude-code-src/src/tools/FileWriteTool/FileWriteTool.ts#L56)：

```typescript
const inputSchema = lazySchema(() =>
  z.strictObject({
    file_path: z.string().describe(
      'The absolute path to the file to write (must be absolute, not relative)',
    ),
    content: z.string().describe('The content to write to the file'),
  }),
)
```

最终转换成给 LLM 的 JSON Schema：

```json
{
  "type": "object",
  "properties": {
    "file_path": {
      "type": "string",
      "description": "The absolute path to the file to write (must be absolute, not relative)"
    },
    "content": {
      "type": "string",
      "description": "The content to write to the file"
    }
  },
  "required": ["file_path", "content"],
  "additionalProperties": false
}
```

### A.6 CC `call()` 关键管道

[FileWriteTool.ts#L223](file:///d:/develop/Workspace/claude-code-src/src/tools/FileWriteTool/FileWriteTool.ts#L223)：

```
1. expandPath(file_path)           → 路径展开（~ → home）
2. discoverSkillDirsForPaths()     → 技能目录发现（跳过，不适用）
3. diagnosticTracker.beforeFileEdited() → LSP 诊断（跳过）
4. fs.mkdir(dir)                   → 确保父目录存在
5. fileHistoryTrackEdit()          → 文件历史备份（跳过，Phase 3）
6. readFileSyncWithMetadata()      → 读取现有文件（判断 create/update）
7. staleness check                 → 对比 mtime vs readFileState（✅ Phase 2 已实现）
8. writeTextContent(path, content, enc, 'LF') → 写入文件（LF 行尾）
9. LSP didChange/didSave           → LSP 通知（跳过）
10. notifyVscodeFileUpdated()      → VSCode 通知（跳过）
11. readFileState.set()            → 更新读取状态（✅ Phase 2 已实现，update_after_write）
12. getPatchForDisplay()           → 生成 diff（update 模式）
13. 返回 { type, filePath, content, structuredPatch, originalFile }
```

### A.7 CC 特性取舍

| CC 特性 | 是否实现 | 理由 |
|---------|---------|------|
| 路径展开（`expandPath`） | ✅ 用 `weakly_canonical` 替代 | C++ 有 `std::filesystem` |
| 父目录创建 | ✅ `create_directories` | 核心功能 |
| 文件存在性判断（create/update） | ✅ `fs::exists` | 核心功能 |
| Pre-read 检查 | ✅ `FileReadStateTracker` + `check_pre_read_and_staleness` | 对齐 CC `validateInput` |
| 文件 staleness 检查 | ✅ mtime 对比 + 内容回退 | 对齐 CC `call()` staleness 逻辑，额外容错云同步 |
| Diff 生成 | ✅ 简化版行级 diff（LCS） | 核心反馈 |
| LSP 集成 | ❌ 不适用 | TUI 项目无 LSP |
| VSCode 通知 | ❌ 不适用 | TUI 项目无 VSCode |
| 技能目录发现 | ❌ 不适用 | 无技能系统 |
| 文件历史备份 | ❌ Phase 3 | 需独立基础设施 |
| Git diff | ❌ Phase 3 | 需 git 集成 |
| Analytics 日志 | ❌ 不适用 | 无分析服务 |
| 行尾处理（强制 LF） | ✅ 直接写入 content | CC 也是直接写入，不重写行尾 |
| UNC 路径安全 | ❌ 暂不处理 | Windows 边缘场景，Phase 3 |
