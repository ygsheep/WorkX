# SkillTool — 技能加载工具

> 按名称加载 `.claude/skills/<name>/SKILL.md` 定义的技能，返回完整指令文本；同时承载整个磁盘 Skill 系统的加载链路。
>
> 对标 Claude Code CLI 的 `SkillTool`（tools/SkillTool/prompt.ts）+ `loadSkillsDir.ts`，在 WorkX 的 ReAct 架构下实现。
>
> 版本：v1.0.0

---

## 一、概述

`SkillTool` 是 Agent 工具集中负责"加载技能"的工具，对应 LLM 工具名 `Skill`。技能由 Markdown 文件定义（frontmatter + 正文 + 可选参考文件），由磁盘加载器在启动时扫描注册。

### 文件清单

| 文件 | 用途 | 版本 |
|------|------|------|
| [skill_tool.h](skill_tool.h) | 工具接口声明（含 registry 延迟注入） | v1.0.0 |
| [skill_tool.cpp](skill_tool.cpp) | 工具实现（按名查找 + 提示词展开） | v1.0.0 |
| [README.md](README.md) | 本文档 | v1.0.0 |

### 依赖的内部模块

| 模块 | 用途 |
|------|------|
| [agent/skill/inclaude/frontmatter.h](../../skill/inclaude/frontmatter.h) | SKILL.md frontmatter 解析（纯函数） |
| [agent/skill/inclaude/skill_loader.h](../../skill/inclaude/skill_loader.h) | 磁盘扫描 + PromptCommand 构建 |
| [agent/skill/inclaude/skill_prompt.h](../../skill/inclaude/skill_prompt.h) | skills 列表 → system prompt 小节 |
| [agent/command/inclaude/registry.h](../../command/inclaude/registry.h) | CommandRegistry 命令注册表 |
| [agent/command/inclaude/command.h](../../command/inclaude/command.h) | PromptCommand（模型执行型命令） |

---

## 二、工具元数据

| 字段 | 值 |
|------|-----|
| name | `Skill` |
| description | Loads a skill by name and returns its full instructions. |
| namespace | `agent::tool` |
| 基类 | `ITool`（IToolMetadata + IToolGuard + IToolCallable） |
| 同步/异步 | 同步返回 `ResultV2<ToolResult>` |
| 线程安全 | `call()` 为 const，registry 读取受 mutex 保护，可跨线程共享 |

---

## 三、输入 Schema

```json
{
  "type": "object",
  "properties": {
    "name": { "type": "string", "description": "The skill name to load" }
  },
  "required": ["name"],
  "additionalProperties": false
}
```

### 字段说明

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `name` | string | 是 | 技能名（SKILL.md 的 `name` 字段或目录名），别名也可 |

---

## 四、Skills 检测链路（三层六步）

Skill 从磁盘到模型上下文要经过三层检测：

### 4.1 发现层（启动时，`register_skill_commands`）

```
① 目录层级检测  find_skill_dirs_up_to_home(cwd)
   └─ 从 cwd 逐级向上到根，每层检测 .claude/skills 是否存在（不限层数；
      home 在路径上时隐式覆盖 ~/.claude/skills）

② 文件检测      load_skills_from_dirs(dirs)
   ├─ 只支持 <name>/SKILL.md 目录格式（单 .md 文件不支持）
   ├─ 目录无 SKILL.md → 跳过
   └─ 解析失败/读取失败 → 跳过，不影响其它 skill

③ 去重检测      weakly_canonical 路径比对
   └─ symlink 指向同一文件只加载一次（首个目录优先）
```

### 4.2 内容层（注册 / 注入时）

```
④ frontmatter 检测  parse_skill_content(content, dir_name)
   ├─ name 缺省 → 用目录名
   ├─ description 缺省 → 从正文首行派生（去掉 # 前缀）
   ├─ user_invocable / disable_model_invocation 解析 bool
   └─ 未闭合 frontmatter → 整篇视为正文

⑤ 过滤检测  build_skills_prompt_section(registry)  → system prompt
   └─ type=prompt && loaded_from==Skills && !disable_model_invocation
      && (description 非空 || when_to_use 有值)
```

### 4.3 运行时层（模型调用 Skill 工具时）

```
⑥ 验证检测  SkillTool::call(input, ctx)
   ├─ name 缺失/为空 → err(InvalidInput)
   ├─ registry 未注入 → err(InvalidInput)
   ├─ 按名查找失败 → err(ResourceNotFound)
   ├─ 命中非 PromptCommand（如本地命令）→ err(InvalidInput)
   └─ 成功 → 展开提示词文本（含 "Base directory for this skill: <dir>" 前缀）
```

> 与 example/cc 的差异：cc 有 5 类来源（managed / user / project / additional /
> legacy-commands），WorkX v1.0.0 仅项目级（cwd 向上遍历，home 在路径上时
> 隐式覆盖用户级）。

---

## 五、输出与错误处理

### 成功输出

```
Base directory for this skill: <skill_dir>

<SKILL.md 正文>
```

`Base directory` 前缀让模型知道参考文件（SKILL.md 同目录及子目录）的位置，可直接用 Read/Grep 读取——与 example/cc 磁盘 skill 契约一致。

### 错误表

| 错误场景 | 错误码 | 错误信息 |
|---------|--------|---------|
| `name` 缺失或为空 | `InvalidInput` | `Missing required argument: name` |
| registry 未注入 | `InvalidInput` | `Skill registry not configured` |
| 技能不存在 | `ResourceNotFound` | `Skill not found: <name>` |
| 命中非 PromptCommand | `InvalidInput` | `Not a skill: <name>` |

---

## 六、使用示例

### 6.1 定义技能（SKILL.md）

```
.claude/skills/workx-build/
├── SKILL.md
└── refs/
    └── commands.md       # 参考文件，模型可从 baseDir 按需读取
```

```markdown
---
name: workx-build
description: 构建 workx（Debug）并运行单元测试
aliases: wb
argument_hint: [target]
when_to_use: 修改了 C++ 源码后需要验证编译通过或运行测试时
---

# Workx Build & Test

1. `cmake --preset default`
2. `cmake --build build --config Debug`
3. `ctest --test-dir build -C Debug --output-on-failure`
```

### 6.2 调用（C++）

```cpp
#include "agent/tool/SkillTool/skill_tool.h"
#include "agent/command/inclaude/registry.h"

auto registry = std::make_shared<command::CommandRegistry>();
// ... registry 中已注册 skills（register_skill_commands）...

tool::SkillTool tool(registry);
ToolContext ctx;
ctx.cwd = "/project";

auto r = tool.call({{"name", "workx-build"}}, ctx);
if (r.is_ok()) {
    std::cout << r.value().text;  // "Base directory for this skill: ..."
}
```

### 6.3 用户侧触发

- 模型：通过 `Skill` 工具按名加载
- 用户：`/workx-build`（或别名 `/wb`）直接展开为提示词发起新一轮查询

---

## 七、与 Claude Code 对比

| 特性 | Claude Code | 本工具 (v1.0.0) | 差异说明 |
|------|-------------|-----------------|----------|
| 工具名 | `Skill` | `Skill` | 一致 |
| SKILL.md 目录格式 | ✅ | ✅ | 一致（`<name>/SKILL.md`） |
| 来源层级 | 5 类（managed/user/project/additional/commands） | 3 类（项目级向上遍历 + `~/.claude/skills` + `~/.workx/skills`） | 简化 |
| baseDir 前缀 | ✅ | ✅ | 一致 |
| aliases | ✅ | ✅ | 注册为同内容命令 |
| frontmatter 字段 | name/description/aliases/argument_hint/when_to_use/model/user_invocable/disable_model_invocation/context/agent/hooks 等 | 上述 11 个 | 一致（超集字段忽略） |
| conditional skills（paths） | ✅ | ✅ | touch 回调 + `<file path>` 引用匹配 |
| bundled skills 程序化注册 | ✅ | ✅ | `register_bundled_skill` |
| MCP skill | ✅ | ❌ | 未实现 |

---

## 八、测试策略

单元测试见 [tests/unit/agent/tool/test_skill_tool.cpp](../../../../tests/unit/agent/tool/test_skill_tool.cpp) 与 [tests/unit/agent/skill/](../../../../tests/unit/agent/skill/)：

| 模块 | 文件 | 覆盖 |
|------|------|------|
| frontmatter | test_skill_frontmatter.cpp | 完整字段/缺省 name/无 frontmatter/未闭合/aliases 格式/context+agent+hooks/bool 变体/CRLF（14 用例） |
| 加载器 | test_skill_loader.cpp | 有效加载/别名共享 prompt/baseDir 前缀/缺 SKILL.md 跳过/name 回退/去重/向上遍历/字段附着（8 用例） |
| 提示词小节 | test_skill_prompt.cpp | 空 registry/列出与描述/context 注入/agent 过滤（8 用例） |
| 钩子执行器 | test_hooks.cpp | shell 执行/失败不中断/空跳过/输出格式化（4 用例） |
| SkillTool | test_skill_tool.cpp | 按名返回/未找到/参数缺失/非 prompt 拒绝（4 用例） |
| 端到端 | test_skill_commands.cpp | register_skill_commands 加载仓库真实 .claude/skills（1 用例） |

---

## 九、设计决策

| 决策 | 理由 |
|------|------|
| Skill 复用 `PromptCommand` + `CommandRegistry` | 零新增注册表，`/skillname` 用户触发与 `Skill` 工具模型触发共用同一数据源 |
| registry 延迟注入（`set_registry`） | CommandRegistry 在 main.cpp 晚于 factory 工具注册创建，先注册后注入 |
| `weakly_canonical` 去重 | 容忍 symlink 场景下同一 SKILL.md 被多个目录发现 |
| frontmatter 手写解析器 | 字段全为扁平标量，不引入 yaml-cpp 依赖；嵌套需求出现再升级 |
| 过滤条件对齐 `getSlashCommandToolSkills` | 仅模型可调用（非 disabled、有描述或 when_to_use/context）且声明 agent 匹配当前 `agent.active` 的 skill 进 system prompt |
| 别名注册为同内容命令 | 共享同一 `PromptGenerator`（std::function 可复制），无需新类型 |

---

## 十、路线图

### v1.0.0（已完成 ✅）

- [x] frontmatter 解析（纯函数，CRLF 兼容）
- [x] 磁盘扫描 + 去重 + 别名
- [x] SkillTool（ITool，错误码齐全）
- [x] system prompt 注入（Available skills 小节）
- [x] 示例技能 `workx-build` + 端到端测试

### 短期

- [x] conditional skills（`paths` frontmatter，touch 匹配文件时激活）
- [x] bundled skills 程序化注册（`register_bundled_skill` + `find_bundled_skills_dir`；随包发布 5 个内置技能 `loop/debug/stuck/verify/batch`，源码 `src/agent/skill/bundled/`，CMake 拷贝到 `<exe_dir>/skills/bundled`，启动优先注册）
- [x] 用户级 `~/.claude/skills` / `~/.workx/skills` 显式支持（home 不在 cwd 祖先路径时，含同名去重）

### 中期

- [x] `hooks` / `context` / `agent` frontmatter 字段
  - `context`：注入 Available skills 小节（`(context: ...)`，description 为空时兜底）
  - `agent`：声明关联 agent；注入与 conditional 激活均按配置 `agent.active` 过滤（不匹配不注入/不激活；用户显式 `/name` 调用不受限）
  - `hooks`：PreActivate 钩子（shell 命令，多行 `- cmd` 或逗号分隔）；用户显式调用（executor）与 conditional 激活（chat_session）时执行，输出并入提示块；单条失败不中断
- [ ] MCP skill 支持
