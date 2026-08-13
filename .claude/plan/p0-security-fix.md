# P0 安全修复：#34 / #35 / #36 实施计划

日期：2026-08-12 ｜ 分支：develop → feature/security-p0

## 目标

三个 P0 安全 issue 的最小可行落地（Simplicity First，纯函数可单测）：

| Issue | 核心交付 |
|---|---|
| #34 文件路径边界 | 统一 `PathValidator`（CWD 边界 + traversal + symlink + 敏感文件 + Windows 可疑模式），接入 Read/Write/Edit 三工具 |
| #35 Shell 无过滤 | `ShellGuard`（危险命令黑名单 + SSRF + env 泄露 + cwd 限制），Bash/PowerShell 接入，沙盒关闭 AskUser 确认，输出密钥脱敏 |
| #36 权限决策层 | 权限模式（default/plan/bypassPermissions）入 ToolContext，各工具实现 check_permissions，可疑操作审计日志 |

## 新组件（agent/tool，纯函数层）

### 1. path_validator.h/.cpp
`validate_path_access(path, cwd, allowlist)` → `ResultV2<void>`：
- 敏感路径拦截（硬编码）：`.env`、`.ssh/`、`.git/config`、`.git/hooks/`、`.bashrc`、`.zshrc`、`credentials`、`.npmrc`、`.aws/`、`.gnupg/`、`id_rsa`、`authorized_keys`、`known_hosts`、`SAM`、`/etc/passwd|shadow`
- Windows 可疑：UNC（`\\`、`//`）、`\\?\` 前缀、8.3 短名（`~` + 数字）、ADS（`::`）、尾点/空格
- CWD 边界：canonical（存在时）/weakly_canonical 路径须在 cwd 或 allowlist 前缀内（symlink 解析后检查，防绕过）

### 2. shell_guard.h/.cpp
- `contains_destructive_command()`：`rm -rf /|~|*`、`mkfs`、`dd if=/dev/`、`format`、`shutdown|reboot|halt|poweroff`、`init 0`、`> /dev/sd` 等
- `is_ssrf_target()`：curl/wget/iwr 指向 169.254.0.0/16、100.64.0.0/10、10/8、172.16/12、192.168/16、`169.254.169.254`
- `leaks_env_vars()`：`env`、`printenv`、`/proc/*/environ`、`Get-ChildItem env:`
- `is_cwd_within(path, base, allowlist)`：cwd 参数须在 ctx.cwd 内

### 3. permission 模式
- `ToolContext` 增加 `permission_mode`（enum：Default/AcceptEdits/Plan/BypassPermissions，默认 Default）
- 新增 `permission_ask.h/.cpp`：`ask_user_confirm(ctx, question)` 复用 AskUserRequestEvent 机制（同 AskUserTool）；无 event_bus → false（fail-closed）
- 决策：Plan → 写/执行 deny；BypassPermissions → 全 allow；Default → 危险操作 ask

## 工具接入

| 工具 | check_permissions | call 内过滤 |
|---|---|---|
| FileReadTool | PathValidator（读） | 读取内容 scan_for_secrets 脱敏（`[REDACTED:label]`）+ 告警 |
| FileWriteTool | PathValidator + plan deny | content scan_for_secrets 拒绝写入 |
| FileEditTool | PathValidator + plan deny | scan_secrets 默认 true（config 默认翻转）+ 拒绝 new_string 含密钥 |
| BashTool | 危险命令 ask（AskUser）；plan deny | shell_guard 过滤 + cwd 限制 + 输出脱敏 + disable_sandbox=true 时 AskUser 确认 |
| PowerShellTool | 同 BashTool | shell_guard 过滤 + 输出脱敏 |

GrepTool/GlobTool 只读搜索，本期不动（后续跟进）。

## 审计

ToolExecutor 已 LOG_INFO 记录调用；补 LOG_WARN 告警（敏感文件访问、危险命令、密钥命中）。

## 测试

- tests/unit/agent/tool/test_path_validator.cpp（traversal/symlink/敏感/UNC/8.3/ADS/尾点/allowlist）
- tests/unit/agent/tool/test_shell_guard.cpp（危险命令/SSRF/env/正常命令放行/cwd 边界）
- 既有工具测试不回归（Bash/FileRead/FileWrite/FileEdit 默认路径）

## 验证

1. `cmake --preset default` + build workx_unit_tests
2. `[tool]` / `[path_validator]` / `[shell_guard]` 全绿
3. 全量 ctest 基线对比（15 失败为预存在环境问题）