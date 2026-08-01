# Code Review 报告 — Issue #13 / PR #18

**审查范围：** PR #18 (`fix/issue-13-filewrite-path` → `develop`)，commit `72d989d`
**对比基线：** `d686695`
**审查立场：** 假设改动有错误，反向举证
**审查结论：** ⚠️ CHANGES_REQUESTED

## 概览

| 项 | 统计 |
|----|------|
| 文件变更 | 3 files / +33 / -20 |
| 新增测试 | 0 cases |
| 测试总数 | 534 cases / 1936 assertions（**3 failed**） |
| mergeable | MERGEABLE (UNSTABLE) |
| 构建 | pass (MSVC Debug，3 个无关 warning) |

## 验收标准核对

| 标准 | 状态 | 说明 |
|------|------|------|
| validate_input 强制校验绝对路径 | ✅ | 三个工具均添加 `is_relative()` 校验 |
| call() 移除相对路径容错 | ✅ | 三个工具均移除 |
| prompt 描述明确绝对路径要求 + 示例 | ✅ | FileWriteTool prompt 更新 |
| 现有单元测试全部通过 | ❌ | **3 个测试失败**（见 H-1） |
| 新增 validate_input 绝对路径校验测试 | ❌ | 未新增 |
| 同步检查 FileEditTool/FileReadTool | ✅ | 三个工具同步修改 |
| 手动验证 glm-4.6v-flash | ⚠️ | 审查中无法执行 |

## ⚠️ High 发现

### H-1. 跨平台路径语义差异导致 3 个测试失败

**位置：** [test_file_edit_tool.cpp#L307](file:///d:/develop/Workspace/workx/tests/unit/agent/tool/test_file_edit_tool.cpp#L307), [L317](file:///d:/develop/Workspace/workx/tests/unit/agent/tool/test_file_edit_tool.cpp#L317), [L328](file:///d:/develop/Workspace/workx/tests/unit/agent/tool/test_file_edit_tool.cpp#L328)

**证据：**

测试用例使用 POSIX 风格路径：
```cpp
// L307
make_edit_input("/tmp/foo", "abc", "abc")
// L317
make_edit_input("/nonexistent/workx_test_path", "old", "new")
// L328
make_edit_input("/tmp/test.ipynb", "", "new content")
```

在 Windows 上，`fs::path("/tmp/foo").is_relative()` 返回 **true**（无盘符的路径被视为相对路径）。

新增的 `is_relative()` 校验（[file_edit_tool.cpp#L243](file:///d:/develop/Workspace/workx/src/agent/tool/FileEditTool/file_edit_tool.cpp#L243)）在 Windows 上拒绝这些路径，导致测试预期与实际不符：

```
failed: r.error().message, ContainsSubstring("does not exist")
  for: "file_path must be an absolute path. Received relative path: '/nonexistent/workx_test_path'..."
```

测试期望错误信息包含 "does not exist"（文件不存在），但实际返回 "must be an absolute path"（校验提前拦截）。

**影响：**
1. **3 个测试失败**，CI 会阻断合并
2. **跨平台行为不一致**：`/tmp/foo` 在 Linux 是绝对路径（通过校验），在 Windows 是相对路径（被拒绝）
3. **错误信息自相矛盾**：错误信息建议 `/home/user/file.txt` 作为正确格式，但在 Windows 上该路径也是 relative

**建议修复（二选一）：**

**方案 A（推荐）：测试用例使用跨平台绝对路径**

```cpp
// 使用 fs::temp_directory_path() 构造跨平台绝对路径
namespace fs = std::filesystem;
fs::path tmp = fs::temp_directory_path() / "workx_test";
make_edit_input(tmp.string(), "abc", "abc")
```

**方案 B：校验逻辑兼容 POSIX 风格路径**

```cpp
// Windows 上也接受以 '/' 开头的路径（POSIX 风格绝对路径）
bool is_absolute_path(const std::string& path) {
    if (fs::path(path).is_absolute()) return true;
    // POSIX 兼容：以 '/' 开头但在 Windows 上被视为 relative
#ifdef _WIN32
    if (!path.empty() && path[0] == '/') return true;
#endif
    return false;
}
```

注意：方案 B 会让 Windows 接受 `/tmp/foo` 但 `weakly_canonical` 会将其解析为 `<当前盘符>:/tmp/foo`，可能不是模型本意。**方案 A 更安全**。

### H-2. 错误信息在 Windows 上自相矛盾

**位置：** [file_write_tool.cpp#L194](file:///d:/develop/Workspace/workx/src/agent/tool/FileWriteTool/file_write_tool.cpp#L194), [file_edit_tool.cpp#L247](file:///d:/develop/Workspace/workx/src/agent/tool/FileEditTool/file_edit_tool.cpp#L247), [file_read_tool.cpp#L111](file:///d:/develop/Workspace/workx/src/agent/tool/FileReadTool/file_read_tool.cpp#L111)

**证据：**

错误信息：
```
"file_path must be an absolute path. Received relative path: '/tmp/foo'.
 Please provide an absolute path like '/home/user/file.txt' or 'C:\\Users\\user\\file.txt'."
```

在 Windows 上：
- `/home/user/file.txt` 是 **relative** 路径（`fs::path("/home/user/file.txt").is_relative() == true`）
- 错误信息建议的"正确格式"在 Windows 上会被同样的校验拒绝

**影响：** 弱模型收到错误信息后，若按建议提供 `/home/user/file.txt`，在 Windows 上仍会被拒绝，形成死循环。

**建议修复：** 错误信息应平台相关：

```cpp
#ifdef _WIN32
    "Please provide an absolute path like 'C:\\Users\\user\\file.txt'."
#else
    "Please provide an absolute path like '/home/user/file.txt'."
#endif
```

## ℹ️ Medium 发现

### M-1. FileReadTool/FileEditTool 的 prompt 未更新示例

**位置：** FileReadTool、FileEditTool 的 prompt() 方法

**证据：** PR 仅更新了 FileWriteTool 的 prompt（添加示例）。FileReadTool 和 FileEditTool 的 prompt/schema 虽然已声明"absolute path"，但未添加格式示例。

**影响：** 弱模型在使用 Read/Edit 工具时仍可能困惑路径格式。

**建议：** 同步更新 FileReadTool、FileEditTool 的 prompt，添加与 FileWriteTool 一致的路径格式示例。

### M-2. 未新增 validate_input 绝对路径校验测试

**证据：** Issue #13 验收标准要求"新增 validate_input 绝对路径校验测试"，但 diff 显示无新增测试文件或用例。

**影响：** 校验逻辑无测试保护，后续重构易引入回归。

**建议：** 新增测试覆盖：
- 绝对路径通过校验
- 相对路径被拒绝（错误信息包含 "absolute path"）
- 空路径被拒绝
- Windows 盘符路径（`C:\...`）通过校验

## Low 发现

### L-1. FileReadTool validate_input 中重复读取 file_path

**位置：** [file_read_tool.cpp#L102-L113](file:///d:/develop/Workspace/workx/src/agent/tool/FileReadTool/file_read_tool.cpp#L102-L113)

**证据：**

```cpp
if (input["file_path"].get<std::string>().empty()) {  // 第一次 get
    return ValidationResult::err(Error::Code::InvalidInput, "file_path must not be empty");
}
{
    const std::string path_str = input["file_path"].get<std::string>();  // 第二次 get
    if (fs::path(path_str).is_relative()) { ... }
}
```

**影响：** 轻微性能开销，代码可读性。

**建议：** 复用 path_str 变量。

## 动态验证结果

| 项 | 结果 |
|----|------|
| MSVC Debug 构建 | ✅ pass（3 个无关 warning） |
| 单元测试 | ❌ **3 failed** / 531 passed（FileEditTool validate_input 3 个用例） |

## 合并建议

**⚠️ 建议修复 H-1/H-2 后合并**

**必须修复（阻断合并）：**
- H-1：修复 3 个失败的测试用例（使用跨平台绝对路径）
- H-2：错误信息平台相关（Windows 不建议 POSIX 风格路径）

**建议修复：**
- M-1：同步更新 FileReadTool/FileEditTool 的 prompt
- M-2：新增 validate_input 绝对路径校验测试

**可选修复：**
- L-1：复用 path_str 变量

## 七维检测速查结果

| 维度 | 结果 | 说明 |
|------|------|------|
| 契约一致性 | ✅ | 三个工具 validate_input/call() 行为一致 |
| 并发与生命周期 | ✅ | 无并发变更 |
| 错误处理 | ⚠️ | H-2 错误信息自相矛盾 |
| 设计与可测试性 | ⚠️ | H-1 测试失败；M-2 无新增测试 |
| 回归风险 | ❌ | 3 个测试失败，跨平台行为变更 |
| 命名与文档 | ✅ | 注释清晰；prompt 更新 |
| 提交规范 | ✅ | `fix(tool): #13` 遵循 Conventional Commits |
