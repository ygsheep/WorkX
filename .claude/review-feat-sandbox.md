# Code Review 报告 — feat/sandbox

**审查范围：** `feat/sandbox` → `develop`，commit `08dedbf`
**对比基线：** `fb72944`（含 feat/subprocess-ripgrep 合并）
**审查立场：** 假设改动有错误，反向举证
**审查结论：** ⚠️ CHANGES_REQUESTED（建议补测试后合并）

## 概览

| 项 | 统计 |
|----|------|
| 文件变更 | 7 files / +1001 / -0 |
| 新增测试 | **0 cases** ⚠️ |
| 测试总数 | 552 cases / 1992 assertions（全通过，但无 sandbox 测试） |
| mergeable | MERGEABLE |
| 构建 | pass（MSVC Debug，无 warning） |
| 关联 Issue | 无 |
| 消费方 | **无**（纯地基，未接入任何 Tool） |

## 验收标准核对

无关联 Issue，按 commit message 声明的交付项核对：

| 交付项 | 状态 | 说明 |
|------|------|------|
| 沙盒命令包装适配器 | ✅ | [sandbox_adapter.cpp](file:///d:/develop/Workspace/workx/src/core/process/sandbox/sandbox_adapter.cpp) 实现 wrap_command() |
| macOS Seatbelt profile 生成 | ✅ | [sandbox_adapter.cpp#L84-L161](file:///d:/develop/Workspace/workx/src/core/process/sandbox/sandbox_adapter.cpp#L84-L161)（条件编译 `__APPLE__`） |
| Linux bwrap 参数生成 | ✅ | [sandbox_adapter.cpp#L172-L218](file:///d:/develop/Workspace/workx/src/core/process/sandbox/sandbox_adapter.cpp#L172-L218)（条件编译 `__linux__`） |
| Windows 降级模式 | ✅ | wrap_command 返回 degraded=true |
| SandboxDetector 探测与缓存 | ✅ | [sandbox_detector.cpp](file:///d:/develop/Workspace/workx/src/core/process/sandbox/sandbox_detector.cpp) 单例 + mutex |
| SandboxConfig 规则配置 | ✅ | restrictive() / permissive() 工厂方法 |
| **单元测试** | ❌ | **无任何测试** |

## ⚠️ High 发现

### H-1. 无单元测试，关键逻辑无保护

**位置：** tests/unit/core/process/ 目录下无 test_sandbox*.cpp

**证据：**
```
tests/unit/core/process/
├── test_subprocess.cpp     ✅
├── test_tool_registry.cpp  ✅
└── (无 test_sandbox*)      ❌
```

`grep -r "sandbox" tests/` 无任何匹配。

**影响：**
1. SandboxConfig::is_permissive() 逻辑无测试（6 个条件判断，易出错）
2. SandboxConfig::restrictive() / permissive() 工厂方法无测试
3. SandboxAdapter::wrap_command() 降级/passthrough 分支无测试
4. SandboxAdapter::make_degraded() / make_passthrough() 无测试
5. SandboxDetector 缓存行为无测试
6. macOS SBPL profile 生成无测试（可在 Windows 上测试降级路径）
7. Linux bwrap 参数生成无测试（可在 Windows 上测试降级路径）

**建议修复：** 新增 `tests/unit/core/process/test_sandbox.cpp`，至少覆盖：

```cpp
// 1. SandboxConfig 工厂方法
TEST_CASE("SandboxConfig::restrictive sets cwd allow", "[sandbox][config]") {
    auto c = SandboxConfig::restrictive("/tmp/proj");
    REQUIRE(c.allow_write.size() == 1);
    REQUIRE(c.allow_write[0] == "/tmp/proj");
    REQUIRE(c.network_isolated);
    REQUIRE(c.allow_system_read);
}

// 2. is_permissive 判断
TEST_CASE("SandboxConfig::is_permissive", "[sandbox][config]") {
    REQUIRE(SandboxConfig::permissive().is_permissive());
    REQUIRE_FALSE(SandboxConfig::restrictive("/tmp").is_permissive());
}

// 3. wrap_command 降级（Windows 或无后端）
TEST_CASE("SandboxAdapter wrap_command degrades on Windows", "[sandbox][adapter]") {
    auto wrapped = SandboxAdapter::wrap_command("rg", {"pat"}, SandboxConfig::restrictive("/tmp"));
#ifdef _WIN32
    REQUIRE(wrapped.degraded);
    REQUIRE_FALSE(wrapped.was_wrapped);
    REQUIRE(wrapped.cmd == "rg");
#else
    // POSIX：取决于 bwrap/sandbox-exec 是否安装
    REQUIRE((wrapped.was_wrapped || wrapped.degraded));
#endif
}

// 4. permissive 配置直接 passthrough
TEST_CASE("SandboxAdapter wrap_command passthrough for permissive", "[sandbox][adapter]") {
    auto wrapped = SandboxAdapter::wrap_command("rg", {"pat"}, SandboxConfig::permissive());
    REQUIRE_FALSE(wrapped.was_wrapped);
    REQUIRE_FALSE(wrapped.degraded);
    REQUIRE(wrapped.cmd == "rg");
    REQUIRE(wrapped.args.size() == 1);
    REQUIRE(wrapped.args[0] == "pat");
}

// 5. SandboxDetector 缓存
TEST_CASE("SandboxDetector caches result", "[sandbox][detector]") {
    auto& d = SandboxDetector::instance();
    d.clear_cache();
    auto first = d.detect();
    auto second = d.detect();
    REQUIRE(first == second);
}
```

## ℹ️ Medium 发现

### M-1. SandboxDetector::path() 双重加锁，detect() 已加锁

**位置：** [sandbox_detector.cpp#L74-L81](file:///d:/develop/Workspace/workx/src/core/process/sandbox/sandbox_detector.cpp#L74-L81), [sandbox_detector.cpp#L114-L119](file:///d:/develop/Workspace/workx/src/core/process/sandbox/sandbox_detector.cpp#L114-L119)

**证据：**
```cpp
Backend SandboxDetector::detect() {
    std::lock_guard<std::mutex> lock(m_mutex);  // 第一次加锁
    if (!m_detected) { m_backend = do_detect(); m_detected = true; }
    return m_backend;
}

std::optional<std::string> SandboxDetector::path() {
    detect();  // 内部加锁
    std::lock_guard<std::mutex> lock(m_mutex);  // 第二次加锁
    return m_path;
}
```

detect() 内部已加锁，path() 先调用 detect()（加锁释放），再单独加锁读取 m_path。虽然功能正确，但两次加锁释放有性能开销。

**实际影响：** 性能轻微；逻辑正确，无死锁（非递归加锁）。

**建议修复：** 重构为单一持锁块：
```cpp
std::optional<std::string> SandboxDetector::path() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_detected) { m_backend = do_detect(); m_detected = true; }
    return m_path;
}
```
或抽取 `detect_locked()` 不持锁版本。

### M-2. do_detect() 在持锁期间调用 fs::exists / access / getenv

**位置：** [sandbox_detector.cpp#L83-L112](file:///d:/develop/Workspace/workx/src/core/process/sandbox/sandbox_detector.cpp#L83-L112)

**证据：** do_detect() 被 detect() 在持锁状态下调用，其中：
- `fs::exists()` / `access()` 是文件系统 I/O
- `getenv()` 不是线程安全的（POSIX）

**影响：** 文件系统 I/O 在持锁期间会阻塞其他线程的 detect() 调用。getenv() 在某些 POSIX 实现中非线程安全。

**实际影响：** 极小（仅首次调用执行 I/O，后续走缓存）；Windows 上 getenv 是线程安全的。

**建议：** 可接受现状（首次调用开销可接受）；若优化可改为双重检查锁定或先探测再加锁。

### M-3. macOS SBPL 网络规则生成顺序有逻辑缺陷

**位置：** [sandbox_adapter.cpp#L142-L158](file:///d:/develop/Workspace/workx/src/core/process/sandbox/sandbox_adapter.cpp#L142-L158)

**证据：**
```cpp
if (config.network_isolated) {
    ss << "(deny network*)\n";
} else {
    ss << "(allow network*)\n";  // 先 allow all
    for (const auto& domain : config.deny_domains) {
        ss << "(deny network* (remote tcp " << ... << "))\n";
    }
    if (!config.allow_domains.empty()) {
        ss << "(deny network*)\n";  // 再 deny all
        for (const auto& domain : config.allow_domains) {
            ss << "(allow network* (remote tcp " << ... << "))\n";
        }
    }
}
```

当 `network_isolated=false` 且 `allow_domains` 非空时：
1. 先输出 `(allow network*)` — 允许所有网络
2. 输出 deny_domains 规则
3. 再输出 `(deny network*)` — 拒绝所有网络
4. 输出 allow_domains 规则

Step 1 的 `(allow network*)` 会被 Step 3 的 `(deny network*)` 覆盖（Seatbelt 后定义优先）。但如果 deny_domains 非空，其规则在 Step 1 和 Step 3 之间，语义混乱。

**影响：** 当同时配置 allow_domains 和 deny_domains 时，profile 行为可能不符合预期。

**建议修复：** 白名单模式优先：
```cpp
if (config.network_isolated) {
    ss << "(deny network*)\n";
} else if (!config.allow_domains.empty()) {
    // 白名单模式：先 deny all，再 allow 指定域名
    ss << "(deny network*)\n";
    for (const auto& domain : config.allow_domains) {
        ss << "(allow network* (remote tcp " << escape_sbpl_string(domain) << "))\n";
    }
} else {
    // 黑名单模式：默认允许，deny 指定域名
    ss << "(allow network*)\n";
    for (const auto& domain : config.deny_domains) {
        ss << "(deny network* (remote tcp " << escape_sbpl_string(domain) << "))\n";
    }
}
```

### M-4. Linux bwrap 未实现 deny_read/deny_write

**位置：** [sandbox_adapter.cpp#L213-L215](file:///d:/develop/Workspace/workx/src/core/process/sandbox/sandbox_adapter.cpp#L213-L215)

**证据：**
```cpp
// deny_write/deny_read 在 bwrap 中难以细粒度实现（bwrap 是命名空间隔离，非路径过滤）
// 此处通过不 bind 对应路径实现（若路径不在 allow_write 中，则保持只读）
// 完整的 deny 规则需要叠加 LD_PRELOAD 或 seccomp，留作后续扩展
```

deny_read/deny_write 被静默忽略，无任何标志告知调用方。

**影响：** 调用方配置 deny_read/deny_write 后以为受保护，但实际未生效。安全敏感场景可能被绕过。

**建议：** 至少在 WrappedCommand 中添加标志或在日志中警告：
```cpp
// 选项 A：添加 warning 字段
struct WrappedCommand {
    // ...
    std::string warning;  // 后端限制导致未完全应用的规则
};

// 选项 B：在 wrap_with_bubblewrap 中 LOG_WARNING
```

## Low 发现

### L-1. find_in_path / is_executable_file 代码重复

**位置：** [sandbox_detector.cpp#L32-L65](file:///d:/develop/Workspace/workx/src/core/process/sandbox/sandbox_detector.cpp#L32-L65) vs [tool_registry.cpp#L48-L86](file:///d:/develop/Workspace/workx/src/core/process/tool_registry.cpp#L48-L86)

**证据：** sandbox_detector.cpp 注释说"避免循环依赖，此处内联简单实现"，但 tool_registry.cpp 的 find_in_path / is_executable 是 static 方法，无循环依赖风险。

**影响：** 代码重复，维护成本。

**建议：** 可接受现状（注释说明了原因）；若优化可抽取为 `core/process/path_utils.h` 共享。

### L-2. 无关联 Issue

同 feat/subprocess-ripgrep 的 L-3，建议后续补 Issue 记录设计决策。

## 动态验证结果

| 项 | 结果 |
|----|------|
| MSVC Debug 构建 | ✅ pass（无 warning） |
| 全量单元测试 | ✅ 552 cases / 1992 assertions 全通过 |
| `[sandbox]` 测试组 | ❌ 不存在（无测试） |

## 跨代码库交叉验证

| 验证项 | 结果 |
|--------|------|
| SandboxAdapter 是否有消费方 | ❌ 无（纯地基） |
| SandboxDetector 是否有消费方 | ❌ 无（仅 SandboxAdapter 内部使用） |
| sandbox 相关测试是否存在 | ❌ 无 |
| Windows 降级路径是否可测 | ✅ 是（wrap_command 在 Windows 上返回 degraded） |

## 合并建议

**⚠️ 建议补测试后合并**

**必须修复（阻断合并）：**
- **H-1：新增单元测试**。sandbox 模块作为安全相关功能，零测试覆盖不可接受。至少覆盖：
  - SandboxConfig 工厂方法（restrictive/permissive）
  - is_permissive() 判断逻辑
  - wrap_command() 降级路径（Windows 可测）
  - wrap_command() passthrough 路径
  - SandboxDetector 缓存行为

**建议修复：**
- M-1：path() 双重加锁优化
- M-3：macOS 网络规则生成顺序修正
- M-4：Linux bwrap deny 规则未实现的警告

**可选修复：**
- M-2：持锁期间 I/O（可接受现状）
- L-1：代码重复（可接受现状）
- L-2：补 Issue

**理由：**
1. 沙盒是安全相关功能，零测试覆盖风险过高
2. macOS 网络规则逻辑有实际缺陷（M-3）
3. Linux deny 规则静默忽略可能导致安全误判（M-4）
4. 代码本身设计合理，平台条件编译清晰，补测试后可合并
