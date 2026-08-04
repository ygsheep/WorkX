# Workx Skill 系统实现计划

## 1. 背景与目标

workx 是类 Claude Code 的 C++ AI CLI（ReAct 循环 + 工具调用 + 权限系统）。`example/cc` 是 Claude Code CLI 源码参考。**workx 目前没有 skill 子系统**（`src/` 下 grep "skill" 无实现代码）。

目标：实现磁盘驱动的最小 Skill 系统，让用户/项目可在 `.claude/skills/<name>/SKILL.md` 定义技能，模型通过工具调用获取，用户可通过 `/skillname` 直接触发。

## 2. 现状（已有落点）

| 能力 | 位置 | 状态 |
|---|---|---|
| `LoadSource::Skills` 枚举 | `src/agent/command/inclaude/types.h:28` | ✅ 已定义未使用 |
| `PromptCommand`（模型执行型命令） | `src/agent/command/inclaude/command.h:130` | ✅ 已实现 |
| `CommandRegistry`（注册/查找/过滤） | `src/agent/command/inclaude/registry.h` | ✅ 已实现 |
| 命令注册入口 | `src/app/command/builtin_commands.cpp` | ✅ 系统命令 |
| `ITool` 三合一接口 | `src/agent/tool/itool.h` | ✅ 已实现 |
| 工具注册入口 | `src/app/factory.cpp:226 register_builtin_tools` | ✅ |
| system prompt | `src/agent/prompt/system_prompt.h` | ⚠️ 仅注释 stub，需定位实际构建处 |

## 3. 设计决策（对照 example/cc）

参考 example/cc 三件套：磁盘加载（`loadSkillsDir.ts`）+ 过滤注入（`getSlashCommandToolSkills`）+ SkillTool（`SkillTool/prompt.ts`）。

**D1 模块布局**：新建 `src/agent/skill/`，仿 command 模块双目录结构：`inclaude/`（头文件）+ `source/`（实现）。

**D2 frontmatter 解析**：手写极简 key-value 解析器（按行 `key: value`，忽略 `---` 分隔符），**不引入 yaml-cpp**（vcpkg.json 无此依赖，避免为一个小功能加依赖）。支持字段：`name`（缺省用目录名）、`description`、`aliases`、`argument_hint`、`when_to_use`、`model`、`user_invocable`、`disable_model_invocation`。

**D3 磁盘扫描**：cwd → 向上到 home 逐级查找 `.claude/skills/`，只支持 `<name>/SKILL.md` 目录格式（对齐 example/cc `loadSkillsFromSkillsDir`）；按 realpath 去重；无 SKILL.md 的目录跳过。

**D4 注册方式**：每个 skill 生成一个 `PromptCommand`，`loaded_from = LoadSource::Skills`，`source = "skills"`，`prompt_generator` 返回 skill markdown 内容；注册进现有 `CommandRegistry`（复用而非新建注册表）。

**D5 SkillTool**：`src/agent/tool/SkillTool/` 实现 `ITool`，输入 `{name}`，输出该 skill 的 markdown 内容（含 baseDir 指引）。模型侧按需拉取，对齐 example/cc 的 Skill 工具语义。

**D6 system prompt 注入**：会话初始化时，把模型可调用 skills 的 `name + description + when_to_use` 列表拼入 system prompt（对齐 `getSlashCommandToolSkills` 的过滤条件：`type=prompt && loaded_from=Skills && (有 description || when_to_use)`）。

**D7 一期不做**（标注扩展点，不实现）：
- Conditional skills（`paths` frontmatter 按文件激活）— 收益低复杂度高
- Bundled skills 程序化注册（`registerBundledSkill`）— 等磁盘版跑通再议
- MCP skill、远程 skill
- frontmatter 中的 `hooks`/`context`/`agent` 字段

## 4. 实施步骤（Goal-Driven）

```
1. [frontmatter 解析器] → verify: 单测覆盖 完整字段/缺省/非法格式/多值 aliases
2. [SkillLoader 扫描]     → verify: 单测覆盖 多层目录/缺 SKILL.md 跳过/去重/向上遍历
3. [SkillTool]           → verify: 单测覆盖 按名返回内容/未找到报错/空参数
4. [接线]                → verify: 编译通过 + 现有全量测试通过
5. [system prompt 注入]  → verify: 会话初始化可见 skills 列表（单测断言 prompt 内容）
6. [示例 skill]          → verify: 仓库根 .claude/skills 放 demo skill，手工 /demo 触发
```

### Step 1: frontmatter 解析器
- 文件：`src/agent/skill/inclaude/frontmatter.h` + `src/agent/skill/source/frontmatter.cpp`
- 接口：`struct SkillFrontmatter { name, description, aliases, argument_hint, when_to_use, model, user_invocable, disable_model_invocation }`；`SkillFrontmatter parse_frontmatter(const std::string& content, const std::string& dir_name)`；纯函数、无 I/O
- 测试：`tests/unit/agent/skill/test_skill_frontmatter.cpp`

### Step 2: SkillLoader
- 文件：`src/agent/skill/inclaude/skill_loader.h` + `src/agent/skill/source/skill_loader.cpp`
- 接口：`std::vector<std::shared_ptr<PromptCommand>> load_skills_from_dirs(const std::vector<std::string>& base_dirs)`（内部：读 `base/name/SKILL.md` → parse_frontmatter → make PromptCommand）；`std::vector<std::string> find_skill_dirs_up_to_home(const std::string& cwd)`
- 复用 `core/utils/` 路径工具（如有）；无则用 `std::filesystem`（C++20，项目已是 C++20）
- 测试：`tests/unit/agent/skill/test_skill_loader.cpp`（Catch2 临时目录 + SOURCE_DIR 常量同现有测试风格）

### Step 3: SkillTool
- 文件：`src/agent/tool/SkillTool/skill_tool.h` + `skill_tool.cpp`
- 构造：`SkillTool(std::shared_ptr<command::CommandRegistry> registry)`；`call()` 按 `name` 查 registry → 取 `PromptCommand::generate_prompt("", ctx)` 文本返回
- 测试：`tests/unit/agent/tool/test_skill_tool.cpp`

### Step 4: 接线
- `src/app/factory.cpp`：`register_builtin_tools` 注册 `SkillTool`（需先持有 CommandRegistry，注意依赖顺序——factory 中 tool_registry 与 command registry 的构造顺序）
- `src/app/main.cpp` 或 `builtin_commands.cpp` 旁：新增 `register_skill_commands(CommandRegistry&, const std::string& cwd)`，会话启动时调用 `load_skills_from_dirs`
- `src/agent/CMakeLists.txt`：追加 2 个 skill 源文件 + SkillTool 源文件
- `tests/unit/CMakeLists.txt`：追加 3 个测试源文件

### Step 5: system prompt 注入
- 定位：`ChatSession`/`ReActLoop` 中实际构建 system prompt 的位置（`system_prompt.h` 是 stub，实施时追踪调用链）
- 实现：追加 "Available skills:" 小节（name + description + when_to_use），并注明"用 SkillTool 获取详细内容"
- 验证：`tests/unit/agent/core/test_chat_session.cpp` 或新建断言 prompt 含 skills 小节

### Step 6: 示例 skill
- 仓库根 `.claude/skills/` 下放 `demo/SKILL.md`（带完整 frontmatter + 正文 + 参考文件）
- 验证命令：`/demo` 触发 + 对话中模型可用 SkillTool 调用

## 5. 构建与验证命令

```powershell
# 配置（已有 build 目录则跳过）
cmake --preset default
# 构建
cmake --build build --config Debug
# 全量测试
ctest --test-dir build -C Debug --output-on-failure
```

## 6. 风险与权衡

- **frontmatter 手写解析器**：不支持嵌套 YAML；当前字段全是扁平标量，够用；若未来需要嵌套结构再引入 yaml-cpp
- **依赖顺序**（Step 4）：SkillTool 需要 CommandRegistry 的 shared_ptr，factory 中两者构造顺序需调整，改动面小
- **system prompt 注入点未知**：`system_prompt.h` 是 stub，Step 5 需先追踪代码；若实际构建点在 react_loop 内，则注入逻辑放那里
- **不实现 conditional/bundled**：一期刻意收敛 scope，避免过度设计（CLAUDE.md 原则 2）
