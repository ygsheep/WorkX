# Conditional Skills 实现计划

**目标**：`paths` frontmatter + touch 事件源回调 + 命中自动激活注入。
**范围**：本计划实现 ① frontmatter `paths` ② touch 回调机制（核心）③ 激活逻辑与注入 ④ system prompt 过滤。

## 背景

- `path_matcher`（`src/agent/tool/path_matcher.h`）已提供 glob 匹配（`*`/`**`/`?`），复用
- `ToolContext::progress_callback` 是既有的"回调注入"先例，touch 回调照此模式
- cc 中 conditional skill：paths 命中时自动注入上下文，用户无需显式调用

## 设计

### 1. touch 事件源（先解决的核心问题）

```
工具执行 → ctx.report_touch(path) → TouchCollector::add(path)（线程安全集合）
用户消息 → send_message 解析 <file path="..."> 标签 → TouchCollector::add(path)
```

- `ToolContext` 新增 `TouchCallback` + `report_touch()`（对齐 ProgressCallback 模式）
- `ReActLoop` 构造新增 `TouchCollector*`（可选指针），react_loop.cpp:558 注入 `ctx.touch_callback`
- `TouchCollector`：mutex + `std::unordered_set<std::string>`（绝对路径），线程安全（工具并行执行）
- BashTool 黑盒命令**不上报**（无法可靠解析 shell 访问的文件），机制为未来 FileRead/FileWrite 类工具预留

### 2. frontmatter

- `SkillFrontmatter` 新增 `std::vector<std::string> paths;`
- 支持 `paths: ["a/*.ts", "b/*.ts"]`（YAML 数组）与逗号分隔两种形式

### 3. 激活与注入

- `activate_conditional_skills(touched, skills)`：touch 路径与 skill.paths 逐条 glob 匹配（POSIX 规范化）
- 命中注入：`send_message` 用户消息前追加
  `[Activated skill: <name>]\nBase directory for this skill: <dir>\n\n<body>`（对齐现有 baseDir 前缀契约）
- 注入位置：作为用户消息文本前缀（与 PromptCommand 展开同一语义）

### 4. system prompt 过滤

- `build_skills_prompt_section`：排除 `paths` 非空的 skill（conditional 不常驻，命中才注入）

## 文件改动

| 文件 | 改动 |
|------|------|
| `src/agent/skill/inclaude/frontmatter.h/.cpp` | paths 解析 |
| `src/agent/tool/context.h` | TouchCallback + report_touch |
| `src/agent/skill/inclaude/conditional.h`（新）/ `source/conditional.cpp`（新） | TouchCollector + activate_conditional_skills |
| `src/agent/core/react_loop.h/.cpp` | 构造注入 TouchCollector* → ctx.touch_callback |
| `src/agent/core/chat_session.h/.cpp` | 成员 TouchCollector；send_message 提取 `<file path>` + 注入激活 skill |
| `src/agent/skill/inclaude/skill_prompt.h` | 过滤 paths 非空 |
| `src/agent/CMakeLists.txt` / `tests/unit/CMakeLists.txt` | conditional.cpp + 测试 |

## 测试（[skill][conditional]）

1. frontmatter：数组/逗号分隔/缺省 paths
2. TouchCollector：多线程 add 后 count 正确
3. 激活匹配：glob 命中/未命中/多路径取交集（任一命中）
4. 链路：mock 工具 report_touch → ReActLoop → collector 收到（含注入 `<file path>` 提取）
5. build_skills_prompt_section 排除 conditional

## 验证

`cmake --build build --config Debug` + `workx_unit_tests.exe "[skill],[conditional]"`
