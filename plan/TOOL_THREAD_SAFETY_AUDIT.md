# 工具线程安全审计报告（Phase 3）

> 关联：ARCH_REFACTOR_PLAN.md §Phase 3 / K-1 / T-2
> 日期：2026-07-26
> 状态：✅ 审计完成，K-1 已修复（方案 A）

## 1. 审计范围

逐个检查 `agent::tool::ITool` 派生类的 `call()` 是否可重入，为 Phase 3.5 并行工具执行扫清障碍。

## 2. 审计结论

| 工具 | 实例可变状态 | 共享状态 | 线程安全 | 并行策略 |
|------|------------|---------|---------|---------|
| BashTool | 无 | 无（未实现） | ✅ 安全（当前 stub） | ⚠️ 实现时需保证 cwd/env 从 ctx 读取，不缓存到实例 |
| FileReadTool | 无 | 无 | ✅ 安全 | 可并行 |
| FileWriteTool | 无 | FileReadStateTracker（mutex 保护）+ FileHistory（mutex 保护） | ✅ 安全 | 可并行（不同文件）；同文件并行写由 staleness 检测 + mtime 兜底 |
| FileEditTool | 无 | 同上 | ✅ 安全 | 同上 |
| GlobTool | 无 | 无 | ✅ 安全 | 可并行 |
| GrepTool | 无 | 无（当前 stub） | ✅ 安全（当前 stub） | ⚠️ 实现时若用 std::regex 需保证 regex 对象线程局部 |
| WebFetchTool | 无 | 无（未实现） | ✅ 安全（当前 stub） | ⚠️ 实现时 HTTP 客户端需线程安全（libcurl 全局初始化在 main 早期完成） |
| MCPTool | 无 | 无（未实现） | ✅ 安全（当前 stub） | ⚠️ 实现时 JSON-RPC 客户端需线程安全或每次 call 创建独立连接 |
| AgentTool | 无 | 无（未实现） | ✅ 安全（当前 stub） | ⚠️ 实现时子 Agent 调度需保证线程安全 |

### 关键发现

1. **所有工具类无实例可变状态**：头文件确认无非 const 成员变量，工具实例本身天然线程安全。
2. **共享状态通过 mutex 保护的单例访问**：
   - `FileReadStateTracker`（`file_read_state.h`）：`mutable std::mutex m_mutex` 保护 `m_states`
   - `FileHistory`（`file_history.h`）：`mutable std::mutex mutex_` 保护 `history_`
3. **未实现工具（BashTool/GrepTool/WebFetchTool/MCPTool/AgentTool）** 当前为 stub，无状态。未来实现时需在 PR review 中重新审计。

## 3. K-1 修复：call() const 化

### 问题

```cpp
// itool.h（修复前）
virtual ToolResult call(const nlohmann::json& input, const ToolContext& ctx) = 0;

// executor.h
class ToolExecutor {
    inline ExecutionResult execute(...) const {
        // tool->call() 非 const，但 execute() 是 const
        exec_result.result = tool->call(input, ctx);
    }
};
```

`ToolExecutor::execute()` 标注 `const` 但 `tool->call()` 非 const，语义不一致。更关键的是：非 const `call()` 允许工具修改实例状态，若未来工具类增加可变成员（如缓存），并行执行同一工具实例会触发数据竞争。

### 方案选择

| 方案 | 描述 | 优点 | 缺点 |
|------|------|------|------|
| **A. call() const 化** | `call()` 改为 `const`，工具用 `mutable` + mutex 保护可变状态 | 编译期保证工具实例可并行调用；最小改动 | 需要修改所有工具 + 测试 Mock |
| B. 工具副本 | 每次并行 `tool_use` 创建工具副本 | 完全隔离 | 工具需支持 clone；FileHistory 等单例状态无法复制 |
| C. 串行有状态工具 | 仅无状态工具并行，有状态工具串行 | 简单 | 当前所有工具无状态，方案退化为 A；未来增加有状态工具时需复杂调度 |

**决策：采用方案 A**。理由：
- 当前所有工具无实例可变状态，const 化零成本
- `mutable` + mutex 是 C++ 标准"逻辑 const"模式，未来扩展性好
- 与 `ToolExecutor::execute() const` 语义一致

### 实施改动

1. `itool.h`：`call()` 声明改为 `virtual ToolResult call(...) const = 0`
2. 9 个工具头文件 + 9 个 cpp 实现：`call()` 签名加 `const`
3. 测试 Mock 工具（3 个测试文件）：`call()` 加 `const override`，观察变量加 `mutable`
4. `executor.h`：`tool->call()` 调用点无需改（`execute()` 已是 const）

## 4. G-1 日志埋点

`ToolExecutor::execute()` 添加 6 处日志埋点：

| 位置 | 级别 | 内容 |
|------|------|------|
| 入口 | DEBUG | tool 名称 + input 大小 |
| 工具未找到 | WARN | tool 名称 |
| 取消前执行 | INFO | tool 名称 |
| 权限拒绝 | WARN | tool 名称 + 拒绝原因 |
| 输入验证失败 | WARN | tool 名称 + 失败原因 |
| 执行完成 | INFO | tool 名称 + is_error + 耗时(ms) |
| 异常捕获 | ERROR | tool 名称 + 异常类型 + what() |

## 5. 后续行动项

- [ ] **Phase 3.5 前置条件已满足**：所有工具 `call()` const 化，可直接用 `std::async` 并行执行
- [ ] 未实现工具实现时需重新审计（在对应 PR 中补充）
- [ ] 若未来工具需要缓存（如 FileReadTool 缓存最近读取的文件 mtime），用 `mutable std::mutex` + `mutable std::unordered_map` 模式
