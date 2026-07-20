# FileEditTool (C++)

> WorkX Agent 的文件编辑工具 —— 通过 `old_string → new_string` 进行精确字符串替换。
>
> 对标 Claude Code `src/tools/FileEditTool/`，使用 C++ 实现的等价能力。
>
> ⚠️ **架构背景**：WorkX Agent 已于 2026-07-16 迁移至 ReAct 架构
> （Thought → Action → Observation 显式三阶段循环，见 [react_loop.cpp](file:///d:/develop/Workspace/workx/src/agent/core/react_loop.cpp)）。
> 本工具通过 [ToolRegistry](file:///d:/develop/Workspace/workx/src/agent/tool/registry.h) 注册后，
> 由 LLM 在 Thought 阶段自主决策调用（原生 function calling）。

---

## 目录

- [1. 项目定位](#1-项目定位)
- [2. 当前状态](#2-当前状态)
- [3. 与 Claude Code 源码对比](#3-与-claude-code-源码对比)
- [4. 实现计划](#4-实现计划)
- [5. 架构设计](#5-架构设计)
- [6. 模块依赖](#6-模块依赖)
- [7. 关键技术决策](#7-关键技术决策)
- [8. 测试策略](#8-测试策略)
- [9. 未来计划](#9-未来计划)
- [10. 源码索引](#10-源码索引)

---

## 1. 项目定位

FileEditTool 是 WorkX Agent 工具系统中的核心文件操作工具之一，负责对本地文件进行**精确字符串替换**。

| 维度 | 内容 |
|---|---|
| **工具名** | `Edit` |
| **核心能力** | 通过 `old_string` → `new_string` 做文件内字符串替换 |
| **替换模式** | 单次替换（默认）/ 全量替换（`replace_all: true`） |
| **强制预读** | 必须先用 `Read` 工具读过该文件，否则报错 |
| **staleness 检测** | 通过 mtime + content 双重校验防止基于过期内容编辑 |
| **文件大小上限** | 1 GiB（防 OOM） |
| **所属命名空间** | `agent::tool` |
| **继承接口** | `ITool` |
| **所属 agent 循环** | ReActLoop（Thought → Action → Observation 三阶段多步迭代） |
| **调度入口** | ToolRegistry（注册表） + ToolExecutor（5 步流水线：查找→取消→权限→校验→执行） |

---

## 2. 当前状态

### 2.0 ReAct 流程接入状态

| 维度 | 状态 | 说明 |
|---|---|---|
| CMake 编译 | ✅ 已纳入 | [CMakeLists.txt#L60](file:///d:/develop/Workspace/workx/CMakeLists.txt#L60) |
| ToolRegistry 注册 | ✅ 已注册 | [main.cpp#L229](file:///d:/develop/Workspace/workx/src/app/main.cpp#L229) `register_tool(std::make_shared<FileEditTool>())` |
| LLM schema 注入 | ✅ 可见 | `ToolRegistry::get_all_schemas()` 自动收集，注入到 LLM `tools` 字段 |
| ReActLoop 可达性 | ✅ 可调用 | LLM 可在 Thought 阶段决策调用，`call()` 执行基础替换流程 |
| 降级行为 | ✅ 兼容 | 替换失败时（未匹配/多匹配）返回结构化错误，LLM 可降级到 `FileWriteTool` |

**当前状态**：FileEditTool 已对 LLM 可见，Phase 1 已实现 —— `call()` 支持基础替换流程
（单次/全量替换 + .bak 备份 + diff 生成 + FileReadStateTracker 集成），`validate_input()`
覆盖 P0 检查（错误码 1/3/4/5/6/7/8/9/10）。后续推进见 [§4 实现计划](#4-实现计划)。

### 2.1 类内部方法状态

| 能力 | 状态 | 说明 |
|---|---|---|
| `name()` / `description()` / `prompt()` | ✅ 完成 | `prompt()` 已对齐 CC 7 条 Usage |
| `input_schema()` | ✅ 完成 | 含 `additionalProperties: false` |
| `call()` | ✅ Phase 3 完成 | 基础替换 + .bak 备份 + diff + 状态刷新 + encoding 保留 + lineEndings 保留 + fileHistory + 引号规范化 |
| `validate_input()` | ✅ Phase 1 完成 | P0 检查已实现（错误码 1/3/4/5/6/7/8/9/10） |
| `check_permissions()` | ❌ 默认通过 | Phase 2+ 任务 |
| 辅助函数 | ✅ Phase 3 完成 | `read_file_lf_normalized` / `count_substring_occurrences` / `replace_all_occurrences` / `check_pre_read_and_staleness` / `create_backup` / `find_actual_string` / `preserve_quote_style` / `write_file_with_encoding` / `FileHistory::save_version` 已实现 |

### 2.2 文件清单

```
FileEditTool/
├── README.md              ← 本文件
├── file_edit_tool.h       ← 类声明 v1.2.0（validate_input + call + 2 私有助手）
└── file_edit_tool.cpp     ← Phase 3 实现（v1.2.0，含 encoding/lineEndings/quote/fileHistory 集成）

tool/                      ← 辅助模块
├── encoding.{h,cpp}       ← 编码检测 + UTF-8 ↔ UTF-16LE/BE/GBK 双向转换
├── line_endings.{h,cpp}   ← LF/CRLF/CR 检测 + 规范化 + 反向应用
├── quote_normalizer.{h,cpp} ← findActualString + preserveQuoteStyle
├── file_history.{h,cpp}   ← 内存多版本备份（支持 undo/多步回滚）
├── path_matcher.{h,cpp}   ← glob 匹配（deny 规则）
└── secret_scanner.{h,cpp} ← gitleaks 规则（secret 检测）
```

---

## 3. 与 Claude Code 源码对比

### 3.1 源码文件映射

| Claude Code (TS) | WorkX (C++) | 状态 |
|---|---|---|
| [FileEditTool.ts](file:///d:/develop/Workspace/claude-code-src/src/tools/FileEditTool/FileEditTool.ts) | [file_edit_tool.cpp](file:///d:/develop/Workspace/workx/src/agent/tool/FileEditTool/file_edit_tool.cpp) | ✅ Phase 1 完成 |
| [types.ts](file:///d:/develop/Workspace/claude-code-src/src/tools/FileEditTool/types.ts) | （分散在 [types.h](file:///d:/develop/Workspace/workx/src/agent/tool/types.h)） | ⚠️ 字段缺失 |
| [prompt.ts](file:///d:/develop/Workspace/claude-code-src/src/tools/FileEditTool/prompt.ts) | （内嵌 file_edit_tool.cpp） | ✅ 对齐 7 条 Usage |
| [constants.ts](file:///d:/develop/Workspace/claude-code-src/src/tools/FileEditTool/constants.ts) | （未独立抽取） | ❌ 缺失 |
| [utils.ts](file:///d:/develop/Workspace/claude-code-src/src/tools/FileEditTool/utils.ts) | （未实现） | ❌ 缺失 |
| [UI.tsx](file:///d:/develop/Workspace/claude-code-src/src/tools/FileEditTool/UI.tsx) | （C++ 无 UI 层） | ➖ 不适用 |

### 3.2 接口映射

| Claude Code (TS) | WorkX (C++) | 差距 |
|---|---|---|
| `name: 'Edit'` | `name()` 返回 `"Edit"` | ✅ 一致 |
| `async description()` | `description()` | ✅ 一致 |
| `async prompt()` 动态拼接 | `prompt()` 静态字符串 | ✅ 已对齐 7 条 Usage（除动态部分） |
| `get inputSchema` (Zod strictObject) | `input_schema()` JSON | ✅ 含 `additionalProperties: false` |
| `async validateInput()` 13 个检查 | `validate_input()` P0 检查 | ⚠️ P0 已实现（错误码 1/3/4/5/6/7/8/9/10），缺 0/2 |
| `async checkPermissions()` | `check_permissions()` 默认通过 | ❌ **未实现** |
| `async call()` 8 阶段管道 | `call()` Phase 3 实现 | ✅ 基础替换 + .bak + diff + 状态刷新 + encoding 保留 + lineEndings + fileHistory + 引号规范化 |
| `mapToolResultToToolResultBlockParam` | 无对应 | ❌ 需在 `ToolResult::to_string()` 中处理 |
| `readFileState: Map` | `FileReadStateTracker` 单例 | ✅ 已集成（`record_read` / `update_after_write`） |
| `getPatchForEdit` (diff 库) | 复用 `FileWriteTool/diff.h` | ✅ LCS 行级 diff 已集成 |
| `findActualString` 引号规范化 | `find_actual_string` | ✅ 已实现（[quote_normalizer.cpp](file:///d:/develop/Workspace/workx/src/agent/tool/quote_normalizer.cpp)） |
| `preserveQuoteStyle` | `preserve_quote_style` | ✅ 已实现（同上） |
| `writeTextContent` 保留 encoding | `write_file_with_encoding` | ✅ 已实现（[encoding.cpp](file:///d:/develop/Workspace/workx/src/agent/tool/encoding.cpp)，UTF-16LE/BE/GBK） |
| `getFileModificationTime` | `std::filesystem::last_write_time` | ✅ 直接对应 |

### 3.3 input_schema 对比

**Claude Code 源码**（[types.ts#L6-19](file:///d:/develop/Workspace/claude-code-src/src/tools/FileEditTool/types.ts#L6)）：

```typescript
z.strictObject({
  file_path:   z.string().describe('The absolute path to the file to modify'),
  old_string:  z.string().describe('The text to replace'),
  new_string:  z.string().describe('The text to replace it with (must be different from old_string)'),
  replace_all: semanticBoolean(z.boolean().default(false).optional())
               .describe('Replace all occurrences of old_string (default false)'),
})
```

**WorkX 当前**（已对齐 CC）：

```cpp
nlohmann::json FileEditTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties", {
            {"file_path",   {{"type","string"},{"description","The absolute path to the file to modify"}}},
            {"old_string",  {{"type","string"},{"description","The text to replace"}}},
            {"new_string",  {{"type","string"},{"description","The text to replace it with (must be different from old_string)"}}},
            {"replace_all", {{"type","boolean"},{"description","Replace all occurrences of old_string (default false)"},{"default",false}}}
        }},
        {"required", {"file_path","old_string","new_string"}},
        {"additionalProperties", false}
    };
}
```

**对齐状态**：
1. ✅ `additionalProperties: false`（对应 `strictObject`）
2. ✅ `new_string` 描述含 `must be different from old_string`
3. ✅ `replace_all` 描述对齐

### 3.4 prompt 对比

**Claude Code 源码**（[prompt.ts#L8-27](file:///d:/develop/Workspace/claude-code-src/src/tools/FileEditTool/prompt.ts#L8)）—— 动态拼接 7 条 Usage：

1. **必须先用 Read 工具**（强制预读）
2. **保留缩进**，行号前缀格式根据 `isCompactLinePrefixEnabled()` 动态切换
3. **优先编辑现有文件**，NEVER 创建新文件除非明确要求
4. **不使用 emoji** 除非用户明确要求
5. **唯一性规则**：`old_string` 不唯一会失败，建议增加上下文或用 `replace_all`
6. **`replace_all` 用于重命名**场景
7. `USER_TYPE === 'ant'` 时追加最小唯一性提示

**WorkX 当前**—— 已对齐 7 条 Usage（除动态部分 #7）：

```cpp
const std::string& FileEditTool::prompt() const {
    // 对齐 Claude Code prompt.ts 的 7 条 Usage（第 7 条 USER_TYPE=ant 跳过，Phase 4）
    static const std::string p{
        "Performs exact string replacements in files.\n"
        "\n"
        "Usage:\n"
        "- You must use your Read tool at least once in the conversation before editing. "
        "This tool will error if you attempt an edit on a file without reading it first.\n"
        "- When editing text from Read tool output, ensure you preserve the exact formatting "
        "(indentation, tabs, newlines) of the original source.\n"
        "- ALWAYS prefer editing existing files in the current working directory. "
        "NEVER create new files unless explicitly required by the user.\n"
        "- Avoid using emojis in files unless explicitly requested by the user.\n"
        "- old_string must be unique within the file unless replace_all is true. "
        "If the string is not unique, the edit will fail. "
        "To make it unique, include more surrounding context in old_string.\n"
        "- Use replace_all to replace all occurrences of old_string with new_string. "
        "This is useful for renaming variables or making sweeping changes across a file.\n"
        "- The file_path parameter must be an absolute path, not a relative path."
    };
    return p;
}
```

**对齐状态**：
1. ✅ #1-#6 已对齐
2. ❌ #7 `USER_TYPE === 'ant'` 动态提示待 Phase 4

---

## 4. 实现计划

### 4.1 阶段划分

```
Phase 1: 基础能力 (MVP)
    │
    ▼
Phase 2: 校验完整性
    │
    ▼
Phase 3: 健壮性增强
    │
    ▼
Phase 4: 高级特性
```

### 4.2 Phase 1 — 基础能力 (MVP)

**目标**：实现最小可用的字符串替换功能。

| 任务 | 优先级 | 验收标准 | 状态 |
|---|---|---|---|
| `input_schema()` 补全 `additionalProperties: false` | P0 | schema 与源码对齐 | ✅ 完成 |
| `prompt()` 改为完整 7 条 Usage 文本 | P0 | 文本与源码一致（除动态部分） | ✅ 完成 |
| `readFileForEdit()` 同步读取 + encoding 检测 | P0 | 支持 UTF-8 / UTF-16LE BOM 检测 | ✅ 完成（`read_file_lf_normalized` + `detect_encoding`） |
| `writeTextContent()` 写入磁盘 | P0 | 保留 encoding + lineEndings | ✅ 完成（`write_file_with_encoding` + `apply_line_ending`） |
| `expandPath()` 路径规范化 | P0 | 支持 `~` 展开 + canonical | ✅ 完成（`weakly_canonical` + `ctx.cwd`） |
| `call()` 基础替换流程 | P0 | 单次替换 + 全量替换可工作 | ✅ 完成 |
| `FileReadStateTracker` 集成 | P0 | 编辑后更新状态 | ✅ 完成（`update_after_write`） |
| 错误处理（文件不存在、未匹配） | P0 | 返回结构化错误 | ✅ 完成 |

### 4.3 Phase 2 — 校验完整性

**目标**：实现 `validate_input()` 的 13 项检查。

| 检查项 | 优先级 | 错误码 |
|---|---|---|
| `old_string === new_string` | P0 | 1 |
| 文件大小 > 1 GiB | P0 | 10 |
| 文件不存在 + `old_string != ""` | P0 | 4 |
| `old_string == ""` + 文件已存在非空 | P0 | 3 |
| `.ipynb` 后缀拒绝 | P1 | 5 |
| 预读检查（FileReadStateTracker） | P0 | 6 |
| staleness 检测（mtime + content） | P0 | 7 |
| `findActualString` 未匹配 | P0 | 8 |
| 多匹配但 `replace_all=false` | P0 | 9 |
| UNC 路径安全跳过 | P2 | — |
| deny 规则匹配 | P2 | 2 |
| settings 文件特殊校验 | P2 | — |
| checkTeamMemSecrets | P3 | 0 |

### 4.4 Phase 3 — 健壮性增强

**目标**：引号规范化、diff 生成、LSP 通知、encoding 保留、lineEndings、fileHistory。

| 任务 | 优先级 | 说明 | 状态 |
|---|---|---|---|
| `findActualString` 引号规范化 | P1 | 弯引号 → 直引号匹配 | ✅ 完成（[quote_normalizer.cpp](file:///d:/develop/Workspace/workx/src/agent/tool/quote_normalizer.cpp)） |
| `preserveQuoteStyle` 保持原引号风格 | P1 | 反向应用到 `new_string` | ✅ 完成（同上） |
| `getPatchForEdit` diff 生成 | P1 | 引入 `dtl` 或自实现 Myers diff | ✅ 完成（复用 `FileWriteTool/diff.h`） |
| `DiffHunk` 强类型 | P1 | 替代 JSON 数组 | ⚠️ 复用 `DiffLine` 结构 |
| encoding 完整支持 | P1 | UTF-8/UTF-16LE/BE/GBK 读写 | ✅ 完成（[encoding.cpp](file:///d:/develop/Workspace/workx/src/agent/tool/encoding.cpp)） |
| lineEndings 保留 | P1 | LF/CRLF/CR 检测 + 写回 | ✅ 完成（[line_endings.cpp](file:///d:/develop/Workspace/workx/src/agent/tool/line_endings.cpp)） |
| LSP didChange/didSave 通知 | P2 | 若有 LSP 集成 | ❌ 未实现（可推迟） |
| fileHistoryTrackEdit 备份 | P2 | 内容 hash 索引的版本历史 | ✅ 完成（[file_history.cpp](file:///d:/develop/Workspace/workx/src/agent/tool/file_history.cpp)，内存版本列表） |

### 4.5 Phase 4 — 高级特性

**目标**：对齐 Claude Code 完整能力。

| 任务 | 优先级 | 说明 |
|---|---|---|
| `USER_TYPE === 'ant'` 最小唯一性提示 | P3 | 内部用户场景 |
| `isCompactLinePrefixEnabled()` 动态前缀 | P3 | 与 Read 工具的行号格式联动 |
| `fetchSingleFileGitDiff` 远程模式 | P3 | 远程 git diff 获取 |
| `validateInputForSettingsFileEdit` | P3 | settings 文件特殊校验 |
| `logFileOperation` / `logEvent` | P3 | 分析事件上报 |
| `notifyVscodeFileUpdated` | P3 | VSCode 集成（若有） |

---

## 5. 架构设计

### 5.1 调用管道

FileEditTool 运行在 ReAct 架构下，调用链分三层：**ReActLoop → ToolExecutor → FileEditTool::call()**。

```
┌──────────────────────────────────────────────────────────────────────┐
│ ReActLoop::run()  ([react_loop.cpp#L177](file:///d:/develop/Workspace/workx/src/agent/core/react_loop.cpp#L177)) │
│  for iteration in 1..max_iterations (默认 25):                       │
│    ┌──────────────────────────────────────────────────────────┐      │
│    │ Thought 阶段                                             │      │
│    │  LLM 流式响应 → 解析 tool_use content block              │      │
│    │  无 tool_use → FinalAnswer，退出循环                     │      │
│    └────────────────────┬─────────────────────────────────────┘      │
│                         ▼ 有 tool_use                                  │
│    ┌──────────────────────────────────────────────────────────┐      │
│    │ Action 阶段 — ToolExecutor::execute(name, input, ctx)    │      │
│    │  Step 1: ToolRegistry::find_by_name("Edit")              │      │
│    │  Step 2: ctx.is_cancelled() 检查                          │      │
│    │  Step 3: ITool::check_permissions()                       │      │
│    │  Step 4: ITool::validate_input() — 13 项检查              │      │
│    │  Step 5: ITool::call() ← 见下方"FileEditTool 内部管道"    │      │
│    └────────────────────┬─────────────────────────────────────┘      │
│                         ▼                                              │
│    ┌──────────────────────────────────────────────────────────┐      │
│    │ Observation 阶段                                          │      │
│    │  ToolResult → ChatMessage::tool_result → push 到 messages │      │
│    │  继续下一轮 Thought（LLM 根据 tool_result 决策）           │      │
│    └──────────────────────────────────────────────────────────┘      │
└──────────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────────┐
│ FileEditTool 内部管道（call() 实现，Phase 3 完成）                    │
│  ├─ expandPath(file_path) 规范化路径（~ 展开 + canonical）            │
│  ├─ readFileForEdit (sync) + encoding 检测（UTF-8 / UTF-16LE/BE / GBK）│
│  ├─ staleness 二次检测（mtime + content，详见 §7.1）                  │
│  ├─ findActualString + preserveQuoteStyle（引号规范化，详见 §7.3）    │
│  ├─ 字符串替换（单次 / replace_all）                                  │
│  ├─ .bak 备份 + FileHistory::save_version（多版本备份）              │
│  ├─ writeTextContent (写入磁盘，保留 encoding + lineEndings)          │
│  ├─ FileReadStateTracker::update_after_write                         │
│  └─ 返回 ToolResult::ok("The file {path} has been updated...")       │
└──────────────────────────────────────────────────────────────────────┘
```

### 5.2 validate_input 决策流程

```
                    ┌─────────────────────────┐
                    │ validate_input(input)   │
                    └────────────┬────────────┘
                                 │
                                 ▼
                    ┌─────────────────────────┐
                    │ old_string ===          │──YES──→ reject (errorCode 1)
                    │ new_string?             │
                    └────────────┬────────────┘
                                 │ NO
                                 ▼
                    ┌─────────────────────────┐
                    │ UNC 路径 (\\ or //)?    │──YES──→ 放行（交给权限层）
                    └────────────┬────────────┘
                                 │ NO
                                 ▼
                    ┌─────────────────────────┐
                    │ fs::file_size > 1 GiB?  │──YES──→ reject (errorCode 10)
                    └────────────┬────────────┘
                                 │ NO
                                 ▼
                    ┌─────────────────────────┐
                    │ readFileForEdit         │
                    └────────────┬────────────┘
                                 │
              ┌──────────────────┴──────────────────┐
              ▼ fileContent === null                ▼ fileContent !== null
   ┌──────────────────────┐              ┌────────────────────────┐
   │ old_string === ''?   │              │ old_string === '' &&   │
   │  YES → 放行 (新建)   │              │ fileContent 非空?      │──YES──→ reject (3)
   │  NO  → reject (4)    │              └──────────┬─────────────┘
   └──────────────────────┘                         │ NO
                                                    ▼
                                         ┌────────────────────────┐
                                         │ endsWith('.ipynb')?    │──YES──→ reject (5)
                                         └──────────┬─────────────┘
                                                    │ NO
                                                    ▼
                                         ┌────────────────────────┐
                                         │ FileReadStateTracker   │──NO───→ reject (6)
                                         │ 存在且非 partial?      │       "File has not been read"
                                         └──────────┬─────────────┘
                                                    │ YES
                                                    ▼
                                         ┌────────────────────────┐
                                         │ lastWriteTime >        │
                                         │ readTimestamp?         │
                                         └──────┬─────────────────┘
                                                │
                                ┌───────────────┴───────────────┐
                                ▼ YES                            ▼ NO
                   ┌─────────────────────────┐              ┌─────────────┐
                   │ 完整读 + content ===    │              │ 放行        │
                   │ lastRead.content?       │              └─────────────┘
                   └──────┬──────────────────┘
                          │
                ┌─────────┴─────────┐
                ▼ YES                ▼ NO
           ┌─────────┐         ┌──────────────┐
           │ 放行    │         │ reject (7)   │
           └─────────┘         │ "modified    │
                               │  since read" │
                               └──────────────┘
                          │
                          ▼ (放行)
              ┌────────────────────────────┐
              │ findActualString 匹配?     │──NO──→ reject (8)
              └────────────┬───────────────┘
                           │ YES
                           ▼
              ┌────────────────────────────┐
              │ matches > 1 &&             │──YES──→ reject (9)
              │ !replace_all?              │
              └────────────┬───────────────┘
                           │ NO
                           ▼
                    ┌──────────────┐
                    │ 放行         │
                    └──────────────┘
```

### 5.3 错误码对照表

| errorCode | 触发条件 | message |
|---|---|---|
| 0 | checkTeamMemSecrets 命中 | （动态密钥提示） |
| 1 | `old_string === new_string` | "No changes to make..." |
| 2 | deny 规则匹配 | "File is in a directory that is denied..." |
| 3 | `old_string === ''` 但文件已存在非空 | "Cannot create new file - file already exists." |
| 4 | 文件不存在且 `old_string !== ''` | "File does not exist..." |
| 5 | `.ipynb` 文件 | "File is a Jupyter Notebook. Use the NotebookEditTool..." |
| 6 | readFileState 不存在或 isPartialView | "File has not been read yet..." |
| 7 | mtime 变化且内容不一致 | "File has been modified since read..." |
| 8 | findActualString 未匹配 | "String to replace not found in file..." |
| 9 | 多匹配但 `replace_all=false` | "Found N matches... but replace_all is false..." |
| 10 | 文件 > 1 GiB | "File is too large to edit..." |

---

## 6. 模块依赖

### 6.1 依赖关系图

```
                    ┌─────────────────────┐
                    │   ReActLoop         │  ← agent 主循环
                    │   react_loop.h      │
                    └──────────┬──────────┘
                               │ 持有
                               ▼
                    ┌─────────────────────┐
                    │   ToolExecutor      │  ← 5 步调度
                    │   executor.h        │
                    └──────────┬──────────┘
                               │ 查找/调度
                               ▼
                    ┌─────────────────────┐
                    │   FileEditTool      │
                    └──────────┬──────────┘
                               │
          ┌────────────────────┼────────────────────┐
          ▼                    ▼                    ▼
┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐
│ ITool (接口)     │  │ FileReadState    │  │ ToolContext      │
│ itool.h          │  │ Tracker          │  │ context.h        │
└──────────────────┘  └──────────────────┘  └──────────────────┘
                               │
                               ▼
                    ┌─────────────────────┐
                    │ std::filesystem     │
                    │ (mtime, file_size)  │
                    └─────────────────────┘

          ┌────────────────────┬────────────────────┐
          ▼                    ▼                    ▼
┌──────────────────┐  ┌──────────────────┐  ┌──────────────────┐
│ ToolResult       │  │ ValidationResult │  │ nlohmann::json   │
│ result.h         │  │ types.h          │  │ (第三方)         │
└──────────────────┘  └──────────────────┘  └──────────────────┘
```

### 6.2 已就绪的运行时依赖

| 模块 | 用途 | 状态 |
|---|---|---|
| [ToolRegistry](file:///d:/develop/Workspace/workx/src/agent/tool/registry.h) | 工具注册表，`get_all_schemas()` 注入 LLM function calling | ✅ 已集成 |
| [ToolExecutor](file:///d:/develop/Workspace/workx/src/agent/tool/executor.h) | 5 步流水线（查找→取消→权限→校验→执行） | ✅ 已集成 |
| [ReActLoop](file:///d:/develop/Workspace/workx/src/agent/core/react_loop.h) | agent 主循环，触发 Action 阶段 | ✅ 已集成 |

### 6.3 未来依赖（Phase 3+）

| 模块 | 用途 | 引入阶段 | 状态 |
|---|---|---|---|
| `dtl` 或自实现 Myers diff | `getPatchForEdit` 生成 diff | Phase 3 | ✅ 已集成（复用 `FileWriteTool/diff.h`） |
| LSP Client | `didChange` / `didSave` 通知 | Phase 3 | ❌ 未实现（可推迟） |
| fileHistory | 备份原内容（版本历史） | Phase 3 | ✅ 已实现（[file_history.cpp](file:///d:/develop/Workspace/workx/src/agent/tool/file_history.cpp)） |
| logging 模块 | `logFileOperation` / `logEvent` | Phase 4 | ❌ 未实现 |
| VSCode 集成层 | `notifyVscodeFileUpdated` | Phase 4（可选） | ❌ 未实现 |

---

## 7. 关键技术决策

### 7.1 为什么 staleness 检测用 mtime + content 双重校验？

**问题**：用户或 linter 可能在 Read 和 Edit 之间修改文件，导致基于过期内容编辑。

**方案**：
1. **mtime 检查**：快速判断是否被修改
2. **content 回退**：mtime 变化但内容一致时放行（如 `touch` 命令）
3. **不一致则拒绝**：强制 LLM 重新 Read

**C++ 实现**：
```cpp
auto lastWriteTime = fs::last_write_time(path);
if (lastWriteTime > state->mtime) {
    if (state->content != currentContent) {
        return ValidationResult::err("File has been modified since read...");
    }
}
```

### 7.2 为什么 Critical Section 在当前架构下不适用？

**历史背景**：Claude Code 原版（TS）运行在 async 环境中，`read → modify → write` 之间可能 yield，
与其他工具的写操作交错，需要 mutex 临界区保护。

**架构现状**（2026-07-16 ReAct 重构后）：
WorkX 已确认采用**同步返回类型**（见项目工程约定：`ToolResult` / `CommandResult` / `ExecutorResult`
均为同步返回，非 `cppcoro::task`）。`ReActLoop` 的 Action 阶段对 `ToolExecutor::execute()` 是
同步阻塞调用，read → modify → write 之间无 yield，天然避免并发交错。

**保留此节的原因**：若未来 `ReActLoop` 改为异步（如多工具并行 Action），需重新评估并发保护策略。
当前不需要任何 mutex。

### 7.3 为什么引号规范化？

**问题**：LLM 生成的 `old_string` 可能用直引号 `'` `"`，但文件中是弯引号 `' ' " "`，导致匹配失败。

**方案**：`findActualString` 先精确匹配，失败后规范化引号再匹配，返回**原始**子串（非规范化后的），保持写回文件时的引号风格。

### 7.4 为什么 `additionalProperties: false`？

**问题**：LLM 可能生成 schema 中不存在的字段（如 `encoding: "utf8"`），导致解析失败或意外行为。

**方案**：JSON Schema 中显式禁止额外字段，对应 Claude Code 的 `z.strictObject`。同时可启用 `strict: true`（Structured Outputs）让 LLM 严格遵循 schema。

### 7.5 为什么文件大小上限 1 GiB？

**问题**：V8 / Bun 字符串长度限制约 2^30 字符（~10 亿），对 ASCII/Latin-1 文件 1 byte = 1 char，超过会 OOM。

**方案**：C++ 无此限制，但为对齐源码行为并防止极端情况，保留 1 GiB 上限。

---

## 8. 测试策略

### 8.1 单元测试用例

| 类别 | 用例 | 期望结果 |
|---|---|---|
| **基础替换** | 单次精确匹配替换 | 成功 |
| **基础替换** | `replace_all=true` 多次替换 | 成功 |
| **基础替换** | `old_string` 含特殊字符（正则元字符） | 成功（字面量匹配） |
| **校验失败** | `old_string === new_string` | reject (1) |
| **校验失败** | 文件不存在 + `old_string != ""` | reject (4) |
| **校验失败** | `old_string == ""` + 文件已存在非空 | reject (3) |
| **校验失败** | 未预读文件 | reject (6) |
| **校验失败** | 文件被修改后编辑 | reject (7) |
| **校验失败** | `old_string` 不匹配 | reject (8) |
| **校验失败** | 多匹配 + `replace_all=false` | reject (9) |
| **校验失败** | 文件 > 1 GiB | reject (10) |
| **校验失败** | `.ipynb` 文件 | reject (5) |
| **引号规范化** | 文件弯引号，输入直引号 | 成功（保留原引号风格） |
| **encoding** | UTF-16LE 文件 | 成功（保留 encoding） |
| **lineEndings** | CRLF 文件 | 成功（保留 CRLF） |
| **路径** | `~` 展开 | 成功 |
| **路径** | 相对路径转绝对 | 成功 |
| **新建文件** | `old_string == ""` + 文件不存在 | 成功创建 |
| **staleness** | mtime 变但内容一致 | 放行 |
| **staleness** | mtime 变且内容不一致 | reject (7) |

### 8.2 集成测试场景

1. **Read → Edit 流程**：先 Read 再 Edit，验证 staleness 通过
2. **Read → 外部修改 → Edit**：验证 staleness 拒绝
3. **Edit → Edit 连续编辑**：验证状态更新正确
4. **Edit → Read**：验证 Read 返回新内容
5. **并发 Edit**：当前架构为同步执行，天然安全；若未来引入并行 Action 需重新评估

---

## 9. 未来计划

### 9.1 短期（Phase 1-2）

- [x] 在 [main.cpp](file:///d:/develop/Workspace/workx/src/app/main.cpp#L229) 中将 FileEditTool 注册到 ToolRegistry
- [x] 完成 `call()` 基础替换流程（单次/全量替换 + .bak 备份 + diff 生成）
- [x] 实现 `validate_input()` P0 检查（错误码 1/3/4/5/6/7/8/9/10）
- [x] 集成 `FileReadStateTracker`（`update_after_write` 写后状态刷新）
- [x] 补全 `input_schema()` 的 `additionalProperties: false`
- [x] `prompt()` 改为完整 7 条 Usage 文本
- [x] 实现 `validate_input()` 剩余项（错误码 0/2：secrets / deny 规则）
  - 错误码 0: [secret_scanner.cpp](file:///d:/develop/Workspace/workx/src/agent/tool/secret_scanner.cpp)（12+ gitleaks 规则，配置开关 `tool.edit.scan_secrets`）
  - 错误码 2: [path_matcher.cpp](file:///d:/develop/Workspace/workx/src/agent/tool/path_matcher.cpp)（glob 匹配 `* / ** / ?`，配置驱动 `tool.edit.deny_patterns`）
- [x] 单元测试覆盖 P0 用例（[test_file_edit_tool.cpp](file:///d:/develop/Workspace/workx/tests/test_file_edit_tool.cpp)，70 个测试用例 / 270 断言）
  - path_matcher：3 个（glob 匹配 + matches_any_pattern + to_posix_path）
  - secret_scanner：4 个（规则命中 + 误报排除 + 去重 + 错误信息）
  - FileEditTool validate_input：8 个（错误码 0/1/2/3/4/5/6/7/8/9 全覆盖）
  - FileEditTool call：5 个（新建 + 单次/全量替换 + 行尾保留）
  - line_endings：5 个（检测 + 应用 + 规范化 + 往返 + 名称）
  - FileEditTool 行尾保留：6 个（CRLF/LF/CR 保留 + replace_all + 无换行）
  - quote_normalizer：8 个（normalize + find/count/preserve 各场景）
  - FileEditTool 引号规范化：4 个（弯引号匹配 + 精确匹配 + validate + 无匹配）
  - encoding：4 个（BOM 检测 + read_file_as_utf8 + write round-trip + encoding_name）
  - FileEditTool 编码保留：5 个（UTF-16LE/BE 保留 + 多字节 + validate + UTF-8 BOM）
  - file_history：6 个（save/get + by_id + latest + clear + 自动裁剪 + 文件独立性）
  - FileEditTool + file_history：3 个（save_version 集成 + 多版本 + undo 模拟）

### 9.2 中期（Phase 3）

- [x] 引号规范化（`findActualString` + `preserveQuoteStyle`）
  - [quote_normalizer.cpp](file:///d:/develop/Workspace/workx/src/agent/tool/quote_normalizer.cpp)（UTF-8 字节级处理，4 种弯引号 ↔ 直引号）
  - 集成到 `validate_input` 错误码 8/9 + `call()` 匹配/写回流程
- [x] diff 生成（复用 `FileWriteTool/diff.h` 的 LCS 算法）
- [x] encoding 完整支持（UTF-8 / UTF-16LE / UTF-16BE / GBK）
  - 新增 [encoding.h](file:///d:/develop/Workspace/workx/src/agent/tool/encoding.h) `read_file_as_utf8` + `write_file_with_encoding`
  - UTF-8 ↔ UTF-16LE/BE 双向转换（含代理对处理）
  - UTF-8 ↔ GBK 平台 API 转换（Windows: MultiByteToWideChar/WideCharToMultiByte）
  - 写回时保留 BOM（UTF-16LE/BE），UTF-8 不保留 BOM（对齐 CC）
- [x] lineEndings 保留（LF / CRLF / CR）
  - [line_endings.cpp](file:///d:/develop/Workspace/workx/src/agent/tool/line_endings.cpp)（多数表决检测 + LF 规范化 + 反向应用）
- [ ] LSP 集成（didChange / didSave 通知）
- [x] fileHistory 备份机制
  - [file_history.h](file:///d:/develop/Workspace/workx/src/agent/tool/file_history.h) + [file_history.cpp](file:///d:/develop/Workspace/workx/src/agent/tool/file_history.cpp)
  - 内存多版本备份（默认上限 20 个/文件，自动裁剪最旧版本）
  - 线程安全（mutex 保护）
  - 集成到 `call()` 写入前 `save_version`，支持 undo/多步回滚

### 9.3 长期（Phase 4）

- [ ] `USER_TYPE === 'ant'` 最小唯一性提示
- [ ] `isCompactLinePrefixEnabled()` 动态前缀（与 Read 工具联动）
- [ ] `fetchSingleFileGitDiff` 远程模式
- [ ] `validateInputForSettingsFileEdit` settings 文件校验
- [ ] `logFileOperation` / `logEvent` 分析事件
- [ ] `notifyVscodeFileUpdated` VSCode 集成
- [ ] 性能基准测试（大文件、多匹配场景）

### 9.4 探索性方向

- [ ] **流式编辑**：对超大文件分块处理，避免全量读入内存
- [ ] **协同编辑**：多 Agent 并发编辑同一文件的冲突解决
- [ ] **语义化 diff**：基于 AST 的智能匹配（如重命名变量时自动更新所有引用）
- [ ] **撤销栈**：集成 fileHistory 实现 undo/redo
- [ ] **编辑建议**：在 LLM 编辑前预览 diff，让用户确认

---

## 10. 源码索引

### 10.1 Claude Code 源码

| 主题 | 源码位置 |
|---|---|
| 工具定义 | [FileEditTool.ts#L86-595](file:///d:/develop/Workspace/claude-code-src/src/tools/FileEditTool/FileEditTool.ts#L86) |
| validateInput | [FileEditTool.ts#L137-362](file:///d:/develop/Workspace/claude-code-src/src/tools/FileEditTool/FileEditTool.ts#L137) |
| call() | [FileEditTool.ts#L387-574](file:///d:/develop/Workspace/claude-code-src/src/tools/FileEditTool/FileEditTool.ts#L387) |
| mapToolResultToToolResultBlockParam | [FileEditTool.ts#L575-594](file:///d:/develop/Workspace/claude-code-src/src/tools/FileEditTool/FileEditTool.ts#L575) |
| readFileForEdit | [FileEditTool.ts#L599-625](file:///d:/develop/Workspace/claude-code-src/src/tools/FileEditTool/FileEditTool.ts#L599) |
| inputSchema / outputSchema | [types.ts](file:///d:/develop/Workspace/claude-code-src/src/tools/FileEditTool/types.ts) |
| 工具描述（prompt） | [prompt.ts](file:///d:/develop/Workspace/claude-code-src/src/tools/FileEditTool/prompt.ts) |
| 常量 | [constants.ts](file:///d:/develop/Workspace/claude-code-src/src/tools/FileEditTool/constants.ts) |
| findActualString / preserveQuoteStyle | [utils.ts](file:///d:/develop/Workspace/claude-code-src/src/tools/FileEditTool/utils.ts) |
| UI 渲染 | [UI.tsx](file:///d:/develop/Workspace/claude-code-src/src/tools/FileEditTool/UI.tsx) |

### 10.2 WorkX C++ 实现

| 主题 | 源码位置 |
|---|---|
| ReActLoop 声明 | [react_loop.h](file:///d:/develop/Workspace/workx/src/agent/core/react_loop.h) |
| ReActLoop 实现 | [react_loop.cpp](file:///d:/develop/Workspace/workx/src/agent/core/react_loop.cpp) |
| ChatSession 委托入口 | [chat_session.cpp](file:///d:/develop/Workspace/workx/src/agent/core/chat_session.cpp) |
| ToolRegistry | [registry.h](file:///d:/develop/Workspace/workx/src/agent/tool/registry.h) |
| ToolExecutor | [executor.h](file:///d:/develop/Workspace/workx/src/agent/tool/executor.h) |
| 工具注册点 | [main.cpp#L225](file:///d:/develop/Workspace/workx/src/app/main.cpp#L225) |
| ITool 接口 | [itool.h](file:///d:/develop/Workspace/workx/src/agent/tool/itool.h) |
| ToolResult | [result.h](file:///d:/develop/Workspace/workx/src/agent/tool/result.h) |
| 输入输出类型 | [types.h](file:///d:/develop/Workspace/workx/src/agent/tool/types.h) |
| ToolContext | [context.h](file:///d:/develop/Workspace/workx/src/agent/tool/context.h) |
| FileEditTool 声明 | [file_edit_tool.h](file:///d:/develop/Workspace/workx/src/agent/tool/FileEditTool/file_edit_tool.h) |
| FileEditTool stub 实现 | [file_edit_tool.cpp](file:///d:/develop/Workspace/workx/src/agent/tool/FileEditTool/file_edit_tool.cpp) |
| FileReadStateTracker | [file_read_state.h](file:///d:/develop/Workspace/workx/src/agent/tool/FileReadState/file_read_state.h) |

### 10.3 相关文档

| 主题 | 文档位置 |
|---|---|
| FileEditTool 完整架构分析 | [docs/cpp-file-edit-tool-analysis.md](file:///d:/develop/Workspace/claude-code-src/docs/cpp-file-edit-tool-analysis.md) |
| Agent 生命周期分析 | [docs/agent-lifecycle-analysis.md](file:///d:/develop/Workspace/claude-code-src/docs/agent-lifecycle-analysis.md) |
| Code Wiki | [docs/code-wiki.md](file:///d:/develop/Workspace/claude-code-src/docs/code-wiki.md) |
