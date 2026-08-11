# CLAUDE.md

Behavioral guidelines to reduce common LLM coding mistakes. Merge with project-specific instructions as needed.

**Tradeoff:** These guidelines bias toward caution over speed. For trivial tasks, use judgment.

## Plan 模式

编写计划时，优先写入项目中的 `.claude/plan/` 目录：

```
.claude/plan/
└── <计划文件>.md    # 计划文件
```

- 使用项目内的 plan 目录，便于版本控制和团队共享
- **查找计划文件时，优先查找 `.claude/plan/` 目录**
- 避免使用 `C:\Users\xxx\.claude\plans\` 路径

---

**Output Format**: Reply in Chinese (中文), end with "喵！"

## 1. Think Before Coding

**Don't assume. Don't hide confusion. Surface tradeoffs.**

Before implementing:
- State your assumptions explicitly. If uncertain, ask.
- If multiple interpretations exist, present them - don't pick silently.
- If a simpler approach exists, say so. Push back when warranted.
- If something is unclear, stop. Name what's confusing. Ask.

## 2. Simplicity First

**Minimum code that solves the problem. Nothing speculative.**

- No features beyond what was asked.
- No abstractions for single-use code.
- No "flexibility" or "configurability" that wasn't requested.
- No error handling for impossible scenarios.
- If you write 200 lines and it could be 50, rewrite it.

Ask yourself: "Would a senior engineer say this is overcomplicated?" If yes, simplify.

## 3. Surgical Changes

**Touch only what you must. Clean up only your own mess.**

When editing existing code:
- Don't "improve" adjacent code, comments, or formatting.
- Don't refactor things that aren't broken.
- Match existing style, even if you'd do it differently.
- If you notice unrelated dead code, mention it - don't delete it.

When your changes create orphans:
- Remove imports/variables/functions that YOUR changes made unused.
- Don't remove pre-existing dead code unless asked.

The test: Every changed line should trace directly to the user's request.

## 4. Goal-Driven Execution

**Define success criteria. Loop until verified.**

Transform tasks into verifiable goals:
- "Add validation" → "Write tests for invalid inputs, then make them pass"
- "Fix the bug" → "Write a test that reproduces it, then make it pass"
- "Refactor X" → "Ensure tests pass before and after"

For multi-step tasks, state a brief plan:
```
1. [Step] → verify: [check]
2. [Step] → verify: [check]
3. [Step] → verify: [check]
```

Strong success criteria let you loop independently. Weak criteria ("make it work") require constant clarification.

---

**These guidelines are working if:** fewer unnecessary changes in diffs, fewer rewrites due to overcomplication, and clarifying questions come before implementation rather than after mistakes.

---

## 版本控制

两层版本体系，**禁止手改版本号**：

### 1. 文件级版本（`src/core`、`src/agent` 文件头 `@version`）

- 修改 `src/core/` 或 `src/agent/` 下的 `.h/.hpp/.cpp` 后，运行
  `python scripts/version_files.py` 升文件版本（小改 **+0.0.1**）
- 大改（公共 API / 行为 / 语义变更）用 `--minor <file>` 标记（**+0.1**）：
  `python scripts/version_files.py --minor src/core/xxx.h`
- `--dry-run` 可只预览不写入；`tui` / `app` 内部文件不强制
- 仅覆盖对外发布的库模块；现有文件无 `@version` 时脚本会自动补 `1.0.0`

### 2. 项目版本（单一事实源 `cmake/version.cmake`）

- 发布时 `python scripts/bump_version.py auto` —— 按文件版本聚合自动升版：
  任一文件大改(+0.1) → **minor**；小改/新增文件数 ≥ 10 → **patch**；都不满足则不升
- 或手动指定 `minor | patch | major`
- 脚本自动同步 `vcpkg.json` / `tests/consumer/vcpkg.json` / `flake.nix` / `nix/workx.nix`；
  cmake configure 期校验不一致会直接 `FATAL_ERROR`

### 3. 二进制溯源

`src/core` / `src/agent` 内嵌 `WORKX_VERSION`（发布版本）、`WORKX_BUILD_INFO`
（`<版本>+<git describe>`）、`WORKX_FILE_VERSION`（文件聚合 `m<小改>M<大改>`）。

命令速查与规则细节见 `.claude/skills/workx-build/`（别名 wb）。
