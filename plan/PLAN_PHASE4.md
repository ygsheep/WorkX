# WorkX 修复 Plan — 阶段 4：代码质量 P2/P3

> **状态**：待审批（未做任何代码修改）
> **范围**：仅修复 P2/P3 级问题，P0/P1 已在阶段 1-3 覆盖
> **依据**：[CODE_REVIEW_REPORT.md](file:///c:\Users\young\Desktop\Develop\WorkX\CODE_REVIEW_REPORT.md) 第 1、3、4、5、6、7、8 章 P2/P3 条目
> **关联阶段**：依赖阶段 1（mutex 引入）、阶段 2（utf8_utils 改造）、阶段 3（API 适配器修复）已完成

> **范围说明**：
> - 阶段 4 仅覆盖 P2/P3 代码质量与维护性问题，不涉及 stub 模块落地（用户已决策"暂时不实现"）
> - 其他模块的剩余 P1（agent/core 2.6-2.13、agent/tool 3.7-3.20、agent/command 5.1-5.8、tui 6.6-6.22、platform 7.2-7.7、core/app 8.9-8.17）留待阶段 5+ 单独处理
> - 用户已决策"`process_bash_command` 仅加 TODO 注释"，本阶段不涉及

---

## 0. 阶段 4 修复范围

### 0.1 总览

| 主题 | 项数 | 主要文件 |
|------|------|---------|
| A. 代码组织与重复消除 | 11 项 | 40+ 文件批量替换 + 6 个工具文件 |
| B. 函数拆分与可维护性 | 4 项 | chat_session.cpp、client.cpp |
| C. 性能优化 | 13 项 | diff.cpp、glob_tool.cpp、http_client.cpp、sse_parser.cpp、task_manager.cpp 等 |
| D. API 与适配器清理 | 10 项 | openai_adapter、anthropic_adapter、sse_parser、provider_preset |
| E. 渲染质量改进 | 12 项 | markdown_renderer.cpp、syntax_highlighter.cpp、line_editor.cpp、chat_renderer.cpp |
| F. 配置与命令系统 | 10 项 | command/command.h、config_manager.h、app_config.cpp、widgets/ |
| G. 平台与命名 | 4 项 | platform_win32.cpp、terminal.cpp、command 子目录 |
| **合计** | **64 项** | **30+ 文件** |

### 0.2 详细问题清单

| 编号 | 级别 | 问题 | 文件 |
|------|------|------|------|
| 1.1 | P3 | namespace 闭合注释全部写 `// namespace workx`，实际为 `agent` | 40+ 文件 |
| 1.3 | P2 | 移植痕迹明显，文件名与目录职责错位 | agent/message/types.h 等 |
| 1.4 | P2 | include 风格不统一（相对路径 vs 项目根路径） | 多处 |
| 1.5 | P3 | `agent/command/inclaude/` 命名混乱 | 目录重命名 |
| 2.14 | P2 | `run_completion` 函数过长（~320 行） | chat_session.cpp |
| 2.15 | P2 | 大量重复代码（backoff 等待、StreamDoneEvent、tool input 解析） | chat_session.cpp |
| 2.16 | P2 | 硬编码 `session_id="default"` | chat_session.cpp |
| 2.17 | P2 | `build_request` 每轮迭代完整拷贝 `m_messages` | chat_session.cpp |
| 2.18 | P2 | parser.h 引号处理不完整 | input/parser.h |
| 2.19 | P2 | `ParsedInput` 应使用 `std::variant` | input/types.h |
| 3.21 | P2 | schema 缺 `additionalProperties: false` | 6 个工具 |
| 3.22 | P2 | LCS 性能 O(n*m) 时间 | diff.cpp |
| 3.23 | P2 | GlobTool `std::regex` 性能差 | glob_tool.cpp |
| 3.24 | P2 | UTF-16 LE/BE 转换代码重复 | encoding.cpp |
| 3.25 | P2 | BOM 跳过逻辑重复 | file_read_tool.cpp + encoding.cpp |
| 3.26 | P2 | LF 规范化逻辑重复 | file_write_tool.cpp |
| 3.27 | P2 | `lines_read` 变量遮蔽 | file_read_tool.cpp |
| 3.28 | P2 | encoding.cpp Unknown 编码回退 UTF-8 | encoding.cpp |
| 3.29 | P3 | constants.h 死代码（PDF_MAX_PAGES_PER_READ） | constants.h |
| 4.29 | P2 | SSE `\r\n\r\n` 分隔符截取长度不严谨 | sse_parser.cpp |
| 4.30 | P2 | poll 线程忙等 | http_client.cpp |
| 4.31 | P2 | `sessions_by_reader` 内存泄漏 | http_client.cpp |
| 4.32 | P2 | `StreamSession::cancel` 不是立即取消 | http_client.cpp |
| 4.33 | P2 | `StreamSession` 析构时 `m_multi` 可能悬空 | http_client.cpp |
| 4.34 | P2 | `get()` 无连接超时、无重试、无 keepalive | http_client.cpp |
| 4.36 | P2 | `SharedPtrWrapper` 设计冗余 | remote_backend.h |
| 4.37 | P2 | `run_stream` 函数过长（160 行） | client.cpp |
| 4.38 | P2 | 两处重复的退避等待代码 | client.cpp |
| 4.39 | P2 | 适配器 URL 尾部斜杠清理重复 | openai/anthropic_adapter |
| 4.40 | P2 | `#ifdef WORKX_HAS_NLOHMANN_JSON` 散落各处 | 多处 |
| 4.41 | P2 | SSEStreamReader `m_content_buffer` 累积但未使用 | sse_stream_reader.cpp |
| 4.42 | P2 | SSEParser 用 `substr` 反复重建缓冲区 | sse_parser.cpp |
| 4.43 | P2 | 每个 SSE chunk 都构造 `std::string` 拷贝 | http_client.cpp |
| 4.44 | P2 | `SSEEvent::has_retry` 判定可疑 | sse_parser.hpp |
| 4.45 | P2 | `i_backend.h` 默认实现里有莫名空行 | i_backend.h |
| 4.46 | P3 | API Key 以明文存储 | chat_types.h |
| 4.47 | P3 | `BackendConfig` 可默认构造、api_key 无校验 | chat_types.h |
| 4.48 | P3 | `provider_type_from_string` 大小写不敏感实现不完整 | provider_type.h |
| 4.49 | P3 | `build_preset_url` 错误处理糟糕 | provider_preset.cpp |
| 4.50 | P3 | `ProviderPreset` 用 string_view 存在悬空引用风险 | provider_preset.h |
| 5.9 | P2 | `PromptBlock::type` 使用字符串而非枚举 | inclaude/types.h |
| 5.10 | P2 | `CommandResult::Compact` 类型未在 executor 中处理 | executor.cpp |
| 5.11 | P2 | `CommandBase` 的 setter 非线程安全 | inclaude/command.h |
| 5.12 | P2 | `ExecutorResult` 的 next_input 是死代码 | inclaude/executor.h |
| 5.13 | P3 | `ProviderPreset` 模型版本可能已过时 | provider_preset.cpp |
| 5.14 | P3 | `find_preset` 使用线性搜索 | provider_preset.cpp |
| 6.23 | P2 | `**bold**` 渲染使用黄色而非粗体 | markdown_renderer.cpp |
| 6.24 | P2 | TableBuffer::feed_line Invalid 状态可能丢失表头 | markdown_renderer.cpp |
| 6.25 | P2 | OutputFormatter bullet 检测未处理 `+` 列表 | output_formatter.cpp |
| 6.26 | P2 | OutputFormatter 表格渲染未考虑缩进 | output_formatter.cpp |
| 6.27 | P2 | syntax_highlighter Diff 行 prefix 用 char 类型 -1 判定 | syntax_highlighter.cpp |
| 6.28 | P2 | classify_by_type 子串匹配可能误判 | syntax_highlighter.cpp |
| 6.29 | P2 | LineEditor::read_line 未重置历史状态 | line_editor.cpp |
| 6.30 | P2 | LineEditor is_special_char 反色显示未考虑多字节 | line_editor.cpp |
| 6.31 | P2 | ChatRenderer 硬编码列位置 | chat_renderer.cpp |
| 6.32 | P2 | SetupWizard 硬编码屏幕尺寸与 ASCII 输入 | setup_wizard.cpp |
| 6.33 | P3 | `Terminal::end_overlay` 使用非标准 `\x1b[s/u` | terminal.cpp |
| 6.34 | P3 | `is_list_item` 中 `dot > 0` 多余 | markdown_renderer.cpp |
| 6.35 | P3 | `std::string buf(code)` 不必要拷贝 | syntax_highlighter.cpp |
| 7.8 | P2 | Win32 SHORT vs int 比较可能溢出 | platform_win32.cpp |
| 7.9 | P2 | Win32 flush 在 VT 模式下是 no-op | platform_win32.cpp |
| 7.10 | P3 | `Terminal::initialize` 使用 USERPROFILE 在非 Windows 不存在 | terminal.cpp |
| 8.18 | P2 | `TaskManager::waitForAll` 使用 busy-wait | task_manager.cpp |
| 8.19 | P2 | StatusBar 每秒重绘 | status_bar.cpp |
| 8.20 | P2 | `ConfigScope::get` 调用不存在的重载 | config_manager.h |
| 8.21 | P2 | app_config 默认路径回退当前目录 | app_config.cpp |
| 8.22 | P2 | CommandPanel/FileSearchPanel 硬编码布局 | widgets/ |
| 8.23 | P2 | SelectPanel 未处理面板超出屏幕 | select_panel.cpp |
| 8.24 | P3 | `m_total_tokens` 未保护负数 | chat_renderer.cpp |

---

## Part A. 代码组织与重复消除（11 项）

### A.1 namespace 闭合注释批量修复（1.1，P3，40+ 文件）

**问题**：40+ 文件实际声明 `namespace agent`，闭合注释写 `} // namespace workx`。

**修复**：批量替换 `} // namespace workx` → `} // namespace agent`。

**实施**：用 Edit 工具的 `replace_all` 单文件多次调用；不修改实际代码逻辑，仅注释。

**风险**：无；纯注释批量替换。

---

### A.2 移植痕迹与文件职责错位（1.3，P2）

**问题**：
- [agent/message/types.h:1-2](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\message\types.h#L1) 文件头注释为 `@file events.h`，实际定义事件类型
- `agent/message/types.h` 含 `UserInputEvent`/`StreamTokenEvent`/`ToolCallEvent`，应归 `events/`
- `agent/message/types.h:79-87` 定义 `ToolType` 枚举，应归 `tool/types.h`
- 多处 "对应参考实现中的 xxx.ts" 注释（疑似从 Claude Code CLI 移植）

**修复方案**：
- 修正 [types.h:1-2](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\message\types.h#L1) 文件头注释为 `@file types.h`、`@brief 消息与事件类型定义`
- **保守方案（推荐）**：不实际移动类型定义（避免大规模 include 改动），仅修正注释，并在文件头 `@file` 注释中说明 "包含事件与消息类型"
- 删除"对应参考实现中的 xxx.ts"注释（搜索全工程 `对应参考实现`/`参考实现` 批量删除）

**风险**：保守方案无风险；激进方案（移动类型）会触发大量 include 修改，不在本次范围。

---

### A.3 include 风格统一（1.4，P2）

**问题**：同一模块内混合使用 `"../inclaude/executor.h"` 相对路径与 `"agent/command/inclaude/registry.h"` 项目根路径。

**修复方案**：统一为项目根路径。批处理：

```
"../inclaude/executor.h" → "agent/command/inclaude/executor.h"
"../inclaude/registry.h" → "agent/command/inclaude/registry.h"
... (其他类似相对路径)
```

**实施**：Grep 搜索 `#include "\.\./` 找出所有相对路径 include，逐一替换。

**风险**：无；CMake include 路径已配置项目根目录。

---

### A.4 BOM 跳过逻辑抽取（3.25，P2）

**问题**：
- [FileReadTool/file_read_tool.cpp:287-296](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\tool\FileReadTool\file_read_tool.cpp#L287) 与 [encoding.cpp:288-297](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\tool\encoding.cpp#L288) 两处 BOM 跳过逻辑完全相同

**修复方案**：抽取到 [encoding.h](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\tool\encoding.h)：

```cpp
// encoding.h 新增
namespace agent {

// 跳过 BOM 并返回检测到的编码
// 输入：文件前 4 字节
// 输出：跳过 BOM 后的起始偏移，编码类型通过 out 参数返回
size_t skip_bom(const std::string& content, std::string& detected_encoding);

} // namespace agent
```

原两处调用统一替换为 `skip_bom(content, encoding)`。

**风险**：无；纯重构。

---

### A.5 LF 规范化逻辑抽取（3.26，P2）

**问题**：[FileWriteTool/file_write_tool.cpp:45-97](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\tool\FileWriteTool\file_write_tool.cpp#L45) `read_file_lf_normalized` 与 `lf_normalize` 共享相同逻辑。

**修复方案**：抽取共用函数：

```cpp
// file_write_tool.cpp 内部或 encoding.h
std::string lf_normalize(const std::string& content);  // 已存在
std::string read_file_lf_normalized(const fs::path& path) {
    std::string content = read_file_content(path);  // 新增辅助
    return lf_normalize(content);
}
```

**风险**：无。

---

### A.6 UTF-16 LE/BE 转换模板化（3.24，P2）

**问题**：[encoding.cpp:32-116](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\tool\encoding.cpp#L32) `utf16le_to_utf8` 和 `utf16be_to_utf8` 几乎完全相同，仅字节序不同。

**修复方案**：模板化或合并：

```cpp
// 内部模板
template <bool LittleEndian>
static std::string utf16_to_utf8_impl(const std::string& src);

// 对外 API 保留两个名字
std::string utf16le_to_utf8(const std::string& src) { return utf16_to_utf8_impl<true>(src); }
std::string utf16be_to_utf8(const std::string& src) { return utf16_to_utf8_impl<false>(src); }
```

**风险**：无；纯重构，行为不变。

---

### A.7 `i_backend.h` 莫名空行清理（4.45，P2）

**问题**：[i_backend.h:43-48](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\i_backend.h#L43) 默认实现里有三行空行，编辑残留。

**修复**：删除多余空行。

**风险**：无。

---

### A.8 ExecutorResult 死代码删除（5.12，P2）

**问题**：[inclaude/executor.h:25-26](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\command\inclaude\executor.h#L25) `next_input`/`submit_next_input` 定义但从未设置，"链式输入"功能未实现。

**修复方案**：
- **保守方案（推荐）**：在头文件中加注释 `// TODO: chain input not yet implemented`，保留字段待后续实现
- **激进方案**：删除字段。若后续要实现需重新设计 API

**本次采用保守方案**：仅加 TODO 注释，不删除字段（避免 API 兼容性问题）。

**风险**：无。

---

### A.9 constants.h 死代码清理（3.29，P3）

**问题**：[constants.h](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\tool\constants.h) `PDF_MAX_PAGES_PER_READ = 20` 定义但 FileReadTool 未实现 PDF 读取。

**修复方案**：删除 `PDF_MAX_PAGES_PER_READ` 常量。

**风险**：无；Grep 确认无引用即可删除。

---

### A.10 `agent/command/inclaude/` 目录命名（1.5，P3）

**问题**：`inclaude` 含义不清（疑似 "include" 笔误或 "in-claude" 缩写）。

**修复方案**：**保留现状**。目录重命名涉及大量 include 修改，且 `inclaude` 在工程内已成约定。仅在 README 或代码规范中记录此命名由来。

**风险**：本次不修改；记录决策。

---

### A.11 `#ifdef WORKX_HAS_NLOHMANN_JSON` 散落统一（4.40，P2）

**问题**：[anthropic_adapter.cpp](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\provider\anthropic_adapter.cpp)、[openai_adapter.cpp](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\provider\openai_adapter.cpp)、[remote_backend.cpp](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\remote\remote_backend.cpp) 多处 `#ifdef WORKX_HAS_NLOHMANN_JSON`，回退路径返回残缺 JSON（空 messages 数组会被 API 拒绝）。

**修复方案**：编译期统一决定。在 [CMakeLists.txt](file:///c:\Users\young\Desktop\Develop\WorkX\CMakeLists.txt) 中：

```cmake
find_package(nlohmann_json CONFIG REQUIRED)
target_compile_definitions(workx PRIVATE WORKX_HAS_NLOHMANN_JSON=1)
```

然后删除所有 `#ifdef WORKX_HAS_NLOHMANN_JSON`/`#else` 回退分支（保留主路径）。

**风险**：构建系统强依赖 nlohmann_json。**缓解**：nlohmann_json 已是项目核心依赖，FetchContent 已配置。

---

## Part B. 函数拆分与可维护性（4 项）

### B.1 `run_completion` 拆分（2.14，P2）

**问题**：[chat_session.cpp:106-424](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\core\chat_session.cpp#L106) `run_completion` ~320 行，单函数承担 agent 循环、流式读取、取消处理、重试、工具执行、消息构建。

**修复方案**：拆分为 4 个私有方法：

```cpp
// chat_session.h 新增私有方法
Result<void, std::string> run_agent_loop(
    const CompletionRequest& request,
    const std::function<bool()>& should_stop);

Result<void, std::string> read_stream_response(
    IStreamReader& reader,
    const std::function<bool()>& should_stop,
    std::string& content_out,
    std::string& reasoning_out,
    ToolUseList& tool_uses_out);

Result<void, std::string> execute_tool_uses(
    const ToolUseList& tool_uses,
    std::vector<ChatMessage>& tool_results_out);

void wait_with_cancel(const std::function<bool()>& should_stop,
                      std::chrono::milliseconds duration);
```

`run_completion` 简化为顶层调度：循环调用上述方法。

**风险**：
- 阶段 1 已加 `m_state_mutex`，拆分需保持锁语义一致
- **缓解**：每个子方法内部加锁，调用方传 const 引用避免双重加锁

---

### B.2 重复代码抽取（2.15 + 4.38，P2）

**问题**：
- backoff 等待：[chat_session.cpp:147-156](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\core\chat_session.cpp#L147) 与 [273-281](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\core\chat_session.cpp#L273) 几乎相同
- StreamDoneEvent 发布：[314-323](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\core\chat_session.cpp#L314) 与 [350-359](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\core\chat_session.cpp#L350) 几乎相同
- tool input JSON 解析：[338-344](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\core\chat_session.cpp#L338) 与 [376-383](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\core\chat_session.cpp#L376) 完全相同
- client.cpp:212-219 与 312-319 完全相同的可中断睡眠（4.38）

**修复方案**：

```cpp
// chat_session.h 私有辅助
void interruptible_sleep(std::chrono::milliseconds duration,
                         const std::function<bool()>& should_stop);
void publish_stream_done(const std::string& content, const std::string& reasoning,
                         bool interrupted, const StreamChunk& chunk);
nlohmann::json parse_tool_input(const std::string& json_str, std::string& err_out);
```

同样在 [client.cpp](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\client.cpp) 内引入 `interruptible_sleep` 私有方法。

**风险**：无；纯重构。

---

### B.3 `run_stream` 拆分（4.37，P2）

**问题**：[client.cpp:178-338](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\client.cpp#L178) `run_stream` 160 行。

**修复方案**：拆分为：

```cpp
// client.h 私有方法
bool handle_submit_failure(int attempt, int delay_ms, const ChatCallbacks& cbs);
void handle_stream_error(int attempt, const ChatCallbacks& cbs);
void wait_with_interrupt(std::chrono::milliseconds duration,
                         const std::function<bool()>& should_stop);
```

**风险**：无；纯重构，行为不变。

---

### B.4 硬编码 `session_id="default"`（2.16，P2）

**问题**：[chat_session.cpp](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\core\chat_session.cpp) 11+ 处硬编码 `"default"`，无法支持多会话。

**修复方案**：

```cpp
// chat_session.h 新增成员
std::string m_session_id = "default";

// 构造函数可指定
ChatSession(..., std::string session_id = "default");

// 所有 publish_async 处替换 "default" → m_session_id
```

**风险**：无；向后兼容。

---

## Part C. 性能优化（13 项）

### C.1 LCS 算法优化（3.22，P2）

**问题**：[diff.cpp:60-77](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\tool\FileWriteTool\diff.cpp#L60) LCS O(n*m) 时间，2 万行文件需 4 亿次比较，秒级延迟。注：阶段 1 已修 OOM（滚动数组），但时间复杂度未改。

**修复方案**：改 Myers O(ND) 算法（D = 编辑距离）。或对相同行做 hash 预处理，相同行跳过比较。

**保守方案（推荐）**：hash 预处理

```cpp
// 行 → hash，相同 hash 的行跳过 LCS 比较
std::vector<size_t> hashes_a(lines_a.size()), hashes_b(lines_b.size());
for (size_t i = 0; i < lines_a.size(); ++i) hashes_a[i] = std::hash<std::string>{}(lines_a[i]);
for (size_t i = 0; i < lines_b.size(); ++i) hashes_b[i] = std::hash<std::string>{}(lines_b[i]);
// 在 LCS 比较中用 hash 比较代替 string 比较
```

**激进方案**：实现 Myers 算法（参考 `diff-match-patch` 或 git diff 实现）。

**本次采用保守方案**：hash 预处理，简单且对相同行密集的文件（如代码）加速明显。

**风险**：hash 碰撞可能漏判差异。**缓解**：hash 相同时再 fallback string 比较。

---

### C.2 GlobTool regex → 手写 matcher（3.23，P2）

**问题**：[glob_tool.cpp:149-188](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\tool\GlobTool\glob_tool.cpp#L149) 用 `std::regex`，大目录（10 万文件）耗时秒级。

**修复方案**：手写 glob matcher（支持 `*`/`?`/`[...]`/`**`）：

```cpp
// glob_tool.cpp 内部
bool glob_match(std::string_view pattern, std::string_view text);
```

实现参考 rsync/glob.c 风格的递归下降匹配器，避免 regex 编译开销。

**风险**：手写 matcher 需充分测试；**缓解**：保留 regex 版本作为 fallback，配置切换。

---

### C.3 poll 线程忙等改 condition_variable（4.30，P2）

**问题**：[http_client.cpp:277-281](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\remote\http_client.cpp#L277) 无传输时每 10ms 轮询 `curl_multi_perform`，长期空转浪费 CPU。

**修复方案**：用 `curl_multi_wait` 替代手动 poll：

```cpp
// 修改前：
// std::this_thread::sleep_for(10ms);
// curl_multi_perform(...);

// 修改后：
int num_fds = 0;
curl_multi_wait(m_multi, nullptr, 0, 1000, &num_fds);
curl_multi_perform(...);
```

`curl_multi_wait` 内部用 select/poll，无传输时阻塞等待，CPU 占用近零。

**风险**：无；`curl_multi_wait` 是 libcurl 推荐 API。

---

### C.4 `sessions_by_reader` 内存泄漏修复（4.31，P2）

**问题**：[http_client.cpp:267-270](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\remote\http_client.cpp#L267) reader 销毁但 session 未完成时 map 条目永不清理。

**修复方案**：在 `cancel_stream`/`on_complete`/析构路径统一清理 `sessions_by_reader`：

```cpp
void HttpClient::cancel_stream(SSEStreamReader* reader) {
    std::lock_guard<std::mutex> lock(m_impl->sessions_mutex);
    auto it = m_impl->sessions_by_reader.find(reader);
    if (it != m_impl->sessions_by_reader.end()) {
        it->second->cancel();
        m_impl->sessions_by_handle.erase(it->second->easy_handle());
        m_impl->sessions_by_reader.erase(it);
    }
}
```

并在 `StreamSession` 析构时检查并通知 HttpClient 清理。

**风险**：需保证锁顺序避免死锁；**缓解**：锁范围最小化。

---

### C.5 StreamSession::cancel 立即取消（4.32，P2）

**问题**：[http_client.cpp:189](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\remote\http_client.cpp#L189) 只设置原子标志，依赖下一次 write 回调返回 0。

**修复方案**：cancel 时同时 `curl_multi_remove_handle`：

```cpp
void StreamSession::cancel() {
    m_cancelled.store(true);
    if (m_added_to_multi.load() && m_multi) {
        curl_multi_remove_handle(m_multi, m_curl);
        m_added_to_multi.store(false);
    }
}
```

**风险**：cancel 后 write 回调不再被调用，需保证 reader 也能收到 finish 信号。**缓解**：cancel 后立即 `reader->finish("cancelled")`。

---

### C.6 StreamSession 析构 m_multi 悬空（4.33，P2）

**问题**：[http_client.cpp:174-180](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\remote\http_client.cpp#L174) shutdown 被调两次时 multi 已 nullptr 但 `m_added_to_multi` 仍为 true → segfault。

**修复方案**：析构前检查 `m_multi != nullptr`：

```cpp
~StreamSession() {
    if (m_added_to_multi.load() && m_multi) {
        curl_multi_remove_handle(m_multi, m_curl);
    }
    if (m_curl) {
        curl_easy_cleanup(m_curl);
    }
}
```

**风险**：无；纯防御性检查。

---

### C.7 `get()` 连接超时与 keepalive（4.34，P2）

**问题**：[http_client.cpp:93-127](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\remote\http_client.cpp#L93) 只设总超时未设连接超时；每次 `get` 都 `curl_easy_init`/`cleanup`，不复用连接。

**修复方案**：

```cpp
// get() 新增
curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);  // 连接超时 10s
curl_easy_setopt(curl, CURLOPT_FRESH_CONNECT, 0L);          // 复用连接
curl_easy_setopt(curl, CURLOPT_FORBID_REUSE, 0L);           // 允许复用
```

**注**：`get()` 是同步短请求，每次 init/cleanup 开销可接受；保留现状，仅加连接超时。

**风险**：无。

---

### C.8 SSEStreamReader buffer 清理（4.41，P2）

**问题**：[sse_stream_reader.cpp:97-103](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\sse_stream_reader.cpp#L97) `m_content_buffer`/`m_reasoning_buffer` 累积但从未被读取，长响应下无限增长占内存。

**修复方案**：直接删除这两个成员：

```cpp
// sse_stream_reader.h 删除
// std::string m_content_buffer;
// std::string m_reasoning_buffer;
```

`feed_data` 后直接通过 parse_cb 解析，不累积。

**风险**：若有外部代码读取这两个 buffer 需同步删除。**缓解**：Grep 搜索 `m_content_buffer`/`m_reasoning_buffer` 确认仅本文件使用。

---

### C.9 SSEParser substr → erase（4.42，P2）

**问题**：[sse_parser.cpp:71-73](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\sse_parser.cpp#L71) 用 `substr` 反复重建缓冲区，O(n) 拷贝。

**修复方案**：改 `erase(0, last_event_end)`：

```cpp
// 修改前：
// m_buffer = m_buffer.substr(last_event_end);

// 修改后：
m_buffer.erase(0, last_event_end);
```

`erase` 仍 O(n) 但避免内存重分配。

**激进方案**：改环形缓冲区（ring buffer），但实现复杂。**本次采用 erase**。

**风险**：无。

---

### C.10 SSE chunk string → string_view（4.43，P2）

**问题**：[http_client.cpp:218](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\remote\http_client.cpp#L218) 每个 SSE chunk 都构造 `std::string` 拷贝，高频流式下拷贝开销显著。

**修复方案**：`feed_data` 改 `string_view`：

```cpp
// sse_stream_reader.h
void feed_data(std::string_view data);

// sse_parser.h
void feed(std::string_view data);
```

**注**：parse_cb 内部仍可能需要 `std::string`（传给 nlohmann::json::parse）。**优化重点**：在 `stream_write_cb` 到 parser 路径避免一次拷贝。

**风险**：string_view 生命周期管理需谨慎；**缓解**：parser 内部立即处理，不持有 view。

---

### C.11 TaskManager::waitForAll busy-wait 改 condition_variable（8.18，P2）

**问题**：[task_manager.cpp:168-181](file:///c:\Users\young\Desktop\Develop\WorkX\src\core\task\task_manager.cpp#L168) `while + sleep_for(10ms)` 忙等。

**修复方案**：

```cpp
// task_manager.h 新增
std::condition_variable m_tasks_cv;
std::mutex m_tasks_mutex;

// Task 完成时通知
void Task::mark_completed() {
    // ...
    m_manager->m_tasks_cv.notify_all();
}

// waitForAll
void TaskManager::waitForAll() {
    std::unique_lock<std::mutex> lock(m_tasks_mutex);
    m_tasks_cv.wait(lock, [this] {
        return m_active_count.load() == 0;
    });
}
```

**风险**：需保证所有 Task 完成路径都 notify；**缓解**：在 Task 析构或 status 变更时统一 notify。

---

### C.12 StatusBar 每秒重绘优化（8.19，P2）

**问题**：[status_bar.cpp:96](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\widgets\status_bar.cpp#L96) 时间戳每秒变化触发全屏重绘。

**修复方案**：仅在状态字段变化时才标记 dirty：

```cpp
// status_bar.h 新增
bool is_dirty() const { return m_dirty; }
void mark_clean() { m_dirty = false; }

// 渲染逻辑
if (timestamp_sec != m_last_rendered_sec || token_count_changed) {
    m_dirty = true;
    render();
    m_last_rendered_sec = timestamp_sec;
}
```

ChatRenderer 检查 `is_dirty()` 决定是否重绘 status_bar。

**风险**：需 ChatRenderer 配合；**缓解**：默认 dirty=true 保证首次渲染。

---

### C.13 `std::string buf(code)` 不必要拷贝（6.35，P3）

**问题**：[syntax_highlighter.cpp:589-592](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\render\syntax_highlighter.cpp#L589) 大文件高亮时频繁堆分配。

**修复方案**：改用栈数组或直接传 char32_t 给 tree-sitter：

```cpp
// 修改前：
// std::string buf(code);
// ts_tree_cursor_...(... buf.c_str() ...);

// 修改后：直接用 code（若已是 string）
// 或栈数组
char buf[5] = {0};
encode_utf8(code, buf);
```

**风险**：无；纯局部优化。

---

## Part D. API 与适配器清理（10 项）

### D.1 SharedPtrWrapper 设计冗余（4.36，P2）

**问题**：[remote_backend.h:23-31](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\remote\remote_backend.h#L23) 为返回 `unique_ptr<IStreamReader>` 而内部用 `shared_ptr` 搞了 wrapper。

**修复方案**：让 `submit_completion` 直接返回 `shared_ptr<IStreamReader>`：

```cpp
// i_backend.h 修改
virtual std::shared_ptr<IStreamReader> submit_completion(const CompletionRequest& request) = 0;

// RemoteBackend 实现
std::shared_ptr<IStreamReader> RemoteBackend::submit_completion(...) {
    // ...
    return reader;  // 直接返回 shared_ptr
}

// Client::run_stream 改用 shared_ptr
auto reader = m_backend->submit_completion(request);
if (!reader) { ... }
```

**删除 SharedPtrWrapper 类**。

**风险**：改动 IBackend 接口，影响所有实现；**缓解**：当前只有 RemoteBackend 一个实现，改动可控。

---

### D.2 SSEEvent::has_retry 判定（4.44，P2）

**问题**：[sse_parser.hpp:26](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\sse_parser.hpp#L26) `retry > 0` 把合法的 `retry: 0`（立即重连）当作未设置。

**修复方案**：改用 `std::optional`：

```cpp
struct SSEEvent {
    // ...
    std::optional<int> retry;  // 缺省表示未设置
    bool has_retry() const { return retry.has_value(); }
};
```

或用 `-1` 哨兵：

```cpp
int retry = -1;  // -1 表示未设置
bool has_retry() const { return retry >= 0; }
```

**本次采用 optional**（更现代）。

**风险**：SSEEvent 结构变化，调用方需同步修改；**缓解**：Grep `has_retry` 找出所有调用点。

---

### D.3 SSE `\r\n\r\n` 分隔符截取（4.29，P2）

**问题**：[sse_parser.cpp:53](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\sse_parser.cpp#L53) 无论分隔符长度都固定减 2，对 `\r\n\r\n` 保留 2 字节分隔符在末尾。

**修复方案**：根据实际匹配的分隔符长度截取：

```cpp
// 修改前：
// m_buffer = m_buffer.substr(pos + 2);

// 修改后：尝试 \r\n\r\n 和 \n\n 两种分隔符
size_t sep_len = 0;
size_t pos = npos;
if ((pos = m_buffer.find("\r\n\r\n")) != npos) sep_len = 4;
else if ((pos = m_buffer.find("\n\n")) != npos) sep_len = 2;

if (pos != npos) {
    std::string event_data = m_buffer.substr(0, pos);
    m_buffer.erase(0, pos + sep_len);
    // ...
}
```

**风险**：无；纯 bug 修复。

---

### D.4 schema 缺 `additionalProperties: false`（3.21，P2）

**问题**：6 个工具 schema 缺 `additionalProperties: false`，FileReadTool/FileWriteTool 已对齐，其他不一致。

**修复方案**：在 FileEditTool/GlobTool/GrepTool/BashTool/AgentTool/MCPTool/WebFetchTool 的 `input_schema` 中加：

```cpp
schema["additionalProperties"] = false;
```

**风险**：LLM 传多余字段时被拒绝；**缓解**：LLM 通常只传 schema 内字段，影响可忽略。

---

### D.5 OpenAI 空 choices usage chunk（4.35，P2）

**注**：本项在阶段 3 第 4 节已修复（OpenAI stream_options + usage），阶段 4 不重复。

---

### D.6 API Key 明文存储（4.46，P3）

**问题**：[chat_types.h:136](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\chat_types.h#L136) `std::string` 析构不擦除内存，可能残留堆中。

**修复方案**：**保留现状**。完整的 secure_string 实现复杂（需 mlock + memset_explicit + 防止编译器优化），且 API Key 在 curl 内部仍会以明文传递。本次仅在文档中记录此限制。

**风险**：本次不修改；记录决策。

---

### D.7 BackendConfig 默认构造与 api_key 校验（4.47，P3）

**问题**：[chat_types.h:125-144](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\api\chat_types.h#L125) 空 api_key 导致 401 但错误信息不直观。

**修复方案**：在 `BackendFactory::create` 或 `Client::create` 中加校验：

```cpp
if (cfg.backend.type == BackendConfig::Type::Remote && cfg.backend.api_key.empty()) {
    return Result<...>::err("API key is required for remote backend");
}
```

**注**：本地 LM Studio 等不需要 api_key，需区分。

**风险**：无。

---

### D.8 `provider_type_from_string` 大小写不敏感（4.48，P3）

**问题**：[model/provider_type.h:34-44](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\model\provider_type.h#L34) 只支持 `"openai"`/`"OpenAI"`/`"OPENAI"` 三种精确匹配，`"OpenAi"` 等会失败。

**修复方案**：转小写后比较：

```cpp
inline ProviderType provider_type_from_string(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (s == "openai") return ProviderType::OpenAI;
    if (s == "anthropic") return ProviderType::Anthropic;
    // ...
    return ProviderType::Unknown;
}
```

**风险**：无。

---

### D.9 `build_preset_url` 错误处理（4.49，P3）

**问题**：[model/provider_preset.cpp:88-104](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\model\provider_preset.cpp#L88) 失败返回字符串 `"(custom URL required)"`，调用方难区分错误消息与合法 URL。

**修复方案**：改返回 `std::optional<std::string>`：

```cpp
std::optional<std::string> build_preset_url(const std::string& provider_name);
// nullopt 表示无预设，需用户手动指定 URL
```

调用方检查 `has_value()` 而非字符串内容。

**风险**：API 变更，调用方需同步修改；**缓解**：Grep `build_preset_url` 找出所有调用点（预计 < 5 处）。

---

### D.10 ProviderPreset string_view 悬空（4.50，P3）

**问题**：[model/provider_preset.h:22-27](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\model\provider_preset.h#L22) 用户从临时 `std::string` 构造时 string_view 指向已销毁对象。

**修复方案**：改用 `const char*` + `std::string_view` 配合静态字符串常量，或直接用 `std::string`：

```cpp
struct ProviderPreset {
    std::string name;            // 改 string_view → string
    ProviderType type;
    std::string default_url;     // 改 string_view → string
    std::string default_model;
    // ...
};
```

**风险**：增加一次字符串拷贝；**缓解**：预设仅 7 个，初始化时拷贝，性能可忽略。

---

## Part E. 渲染质量改进（12 项）

### E.1 `**bold**` 改用粗体（6.23，P2）

**问题**：[markdown_renderer.cpp:429-437](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\render\markdown_renderer.cpp#L429) 输出 `ansi::YELLOW` 而非 `ansi::BOLD`，违反 Markdown 语义。

**修复方案**：

```cpp
// 修改前：
// out += ansi::YELLOW;

// 修改后：
out += ansi::BOLD;
```

**风险**：无；视觉变化但更符合 Markdown 语义。

---

### E.2 TableBuffer::feed_line Invalid 修复（6.24，P2）

**问题**：[markdown_renderer.cpp:308-343](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\render\markdown_renderer.cpp#L308) 首行因前导空行被识别为 Invalid 时整张表渲染失败。

**修复方案**：Invalid 状态下若已缓存表头，继续保留并在后续有效行到达时重新解析：

```cpp
if (state == State::Invalid) {
    if (!m_header.empty()) {
        // 已有缓存表头，转 Invalid 状态但保留表头
        m_state = State::Invalid;
        return;
    }
    // 完全无效，清空
}
```

**风险**：需充分测试嵌套表格场景；**缓解**：保守处理，仅在表头已存在时保留。

---

### E.3 OutputFormatter bullet `+` 列表（6.25，P2）

**问题**：[output_formatter.cpp:124-132](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\render\output_formatter.cpp#L124) 只识别 `-`/`*`，CommonMark 允许 `-`/`*`/`+`。

**修复方案**：

```cpp
// 修改前：
// if (line[0] == '-' || line[0] == '*') {

// 修改后：
if (line[0] == '-' || line[0] == '*' || line[0] == '+') {
```

**风险**：无。

---

### E.4 OutputFormatter 表格缩进（6.26，P2）

**问题**：[output_formatter.cpp:285-289](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\render\output_formatter.cpp#L285) 嵌套在引用块/列表中的表格宽度计算错误。

**修复方案**：传入 indent 参数，计算列宽时减去 indent：

```cpp
void render_table(const std::vector<std::vector<std::string>>& rows, int indent);
// 计算宽度时用 term_width - indent
```

**风险**：需调用方传 indent；**缓解**：默认 indent=0 保持向后兼容。

---

### E.5 syntax_highlighter Diff prefix char 类型（6.27，P2）

**问题**：[syntax_highlighter.cpp:478](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\render\syntax_highlighter.cpp#L478) `prefix` 为 `char`，`-1` 等于 `0xFF`，UTF-8 中字节 `0xFF` 可能误判。

**修复方案**：改用 `int` 或 `enum class DiffPrefix`：

```cpp
enum class DiffPrefix : int {
    None = 0,
    Add = 1,    // '+'
    Del = 2,    // '-'
    Hunk = 3,   // '@'
    Header = 4  // 'd' (---/+++) 等
};

DiffPrefix prefix = DiffPrefix::None;
```

**风险**：影响 syntax_highlighter 内部多处；**缓解**：本地化修改，不暴露到外部。

---

### E.6 classify_by_type 子串匹配（6.28，P2）

**问题**：[syntax_highlighter.cpp:285-304](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\render\syntax_highlighter.cpp#L285) `find("number")` 同时匹配 `"number_literal"` 和 `"number"`。

**修复方案**：改用精确匹配：

```cpp
// 修改前：
// if (type.find("number") != npos)

// 修改后：
if (type == "number" || type == "number_literal" || type == "float" || type == "integer") {
    // number 高亮
}
```

**风险**：无；更精确的类型匹配。

---

### E.7 LineEditor::read_line 重置历史状态（6.29，P2）

**问题**：[line_editor.cpp:337](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\input\line_editor.cpp#L337) 进入循环未重置 `m_history_idx`/`m_backup_line`，上次浏览到历史中间项后新输入从中间项开始。

**修复方案**：

```cpp
std::string LineEditor::read_line(...) {
    m_history_idx = -1;        // 新增：重置
    m_backup_line.clear();     // 新增
    // ... 原逻辑
}
```

**风险**：无。

---

### E.8 LineEditor is_special_char 多字节（6.30，P2）

**问题**：[line_editor.cpp:562-569](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\input\line_editor.cpp#L562) UTF-8 序列可能被拆开显示。

**修复方案**：用阶段 2 抽取的 `char32_width` 判定字符宽度后再决定反色：

```cpp
// 阶段 2 已加 char32_width 函数
if (char32_width(codepoint) > 0) {
    // 完整字符，正常显示
} else {
    // 控制字符，反色
}
```

**风险**：无；依赖阶段 2 完成的基础设施。

---

### E.9 ChatRenderer 硬编码列位置（6.31，P2）

**问题**：[chat_renderer.cpp:502-506](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\render\chat_renderer.cpp#L502) `cursor_to_pos(input_row, 3)` 硬编码列 3。

**修复方案**：抽取常量或动态计算：

```cpp
// chat_renderer.h
static constexpr int INPUT_PROMPT_COL = 3;

// 使用
cursor_to_pos(input_row, INPUT_PROMPT_COL);
```

**风险**：无；纯可维护性改进。

---

### E.10 SetupWizard 硬编码屏幕尺寸与 ASCII 输入（6.32，P2）

**问题**：
- [setup_wizard.cpp:57](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\setup\setup_wizard.cpp#L57) `m_screen->resize(80, 30)` 假定 80x30
- [286-289](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\setup\setup_wizard.cpp#L286) 退格 `write("\b \b")` 假定字符宽 1
- [294](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\setup\setup_wizard.cpp#L294) `ch >= 0x20 && ch < 0x7F` 只接受 ASCII，无法输入中文 API Key

**修复方案**：

```cpp
// 1. 用实际终端尺寸
auto [w, h] = m_terminal->size();
m_screen->resize(w, h);

// 2. 退格用 char32_width 判定
int width = char32_width(ch);
for (int i = 0; i < width; ++i) {
    write("\b \b");
}

// 3. 接受 UTF-8 多字节
if (ch >= 0x20 && ch != 0x7F) {  // 移除上限 0x7F
    // 接受任意 Unicode（除 DEL）
}
```

**风险**：需测试中文 API Key 输入；**缓解**：阶段 2 已完善 UTF-8 处理。

---

### E.11 Terminal::end_overlay DECSC/DECRC（6.33，P3）

**问题**：[terminal.cpp:528-553](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\core\terminal.cpp#L528) 使用 `\x1b[s/u`，部分终端不支持。

**修复方案**：改 `\x1b7`/`\x1b8`（DECSC/DECRC）：

```cpp
// 修改前：
// write("\x1b[s");
// write("\x1b[u");

// 修改后：
write("\x1b7");  // DECSC
write("\x1b8");  // DECRC
```

**风险**：无；DECSC/DECRC 是 VT100 标准，兼容性更好。

---

### E.12 is_list_item dot > 0 多余 + buf(code) 拷贝（6.34 + 6.35，P3）

**问题**：
- [markdown_renderer.cpp:536-540](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\render\markdown_renderer.cpp#L536) `size_t` 无符号，`dot > 0` 永真
- [syntax_highlighter.cpp:589-592](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\render\syntax_highlighter.cpp#L589) 已在 C.13 修复

**修复方案**：

```cpp
// is_list_item 修改：
// 修改前：
// if (dot != std::string::npos && dot > 0)

// 修改后（dot > 0 多余，npos 已经是最大值）：
if (dot != std::string::npos)
```

**风险**：无。

---

## Part F. 配置与命令系统（10 项）

### F.1 PromptBlock::type 改 enum class（5.9，P2）

**问题**：[inclaude/types.h:67-71](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\command\inclaude\types.h#L67) `std::string` 易拼写错误。

**修复方案**：

```cpp
enum class PromptBlockType {
    System, User, Context, Examples, History, Other
};

struct PromptBlock {
    PromptBlockType type;
    std::string content;
};

// 解析时映射
PromptBlockType from_string(std::string_view s) {
    if (s == "system") return PromptBlockType::System;
    if (s == "user") return PromptBlockType::User;
    // ...
}
```

**风险**：调用方需同步修改；**缓解**：Grep `PromptBlock` 找出所有使用点。

---

### F.2 CommandResult::Compact 处理（5.10，P2）

**问题**：[inclaude/types.h:48,54](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\command\inclaude\types.h#L48) 定义 `Compact` 类型但 executor 完全未处理。

**修复方案**：**保守方案（推荐）**：在 executor 中加 TODO 注释，标记 Compact 类型未实现：

```cpp
case CommandResult::Type::Compact:
    // TODO: Compact command not yet implemented (depends on agent/compact/ stub)
    LOG_WARN("Compact command not yet implemented");
    break;
```

注：用户已决策 14 个 stub 模块"暂时不实现"，Compact 依赖 compact/ 模块，本次仅加 TODO。

**风险**：无。

---

### F.3 CommandBase setter 线程安全（5.11，P2）

**问题**：[inclaude/command.h:80-82](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\command\inclaude\command.h#L80) 跨线程修改 `is_enabled_`（std::function）会数据竞争。

**修复方案**：

```cpp
class CommandBase {
private:
    mutable std::mutex m_mutex;
    std::atomic<bool> is_enabled_{true};
    // generator_ 仍用 mutex 保护
    
public:
    void set_enabled(bool enabled) { is_enabled_.store(enabled); }
    bool is_enabled() const { return is_enabled_.load(); }
    
    void set_generator(std::function<std::vector<PromptBlock>()> gen) {
        std::lock_guard<std::mutex> lock(m_mutex);
        generator_ = std::move(gen);
    }
    // ...
};
```

**风险**：无；纯加锁。

---

### F.4 ConfigScope::get 重载问题（8.20，P2）

**问题**：[config_manager.h:172-180](file:///c:\Users\young\Desktop\Develop\WorkX\src\core\config\config_manager.h#L172) `ConfigManager::get<T>(key, default_value)` 重载不存在。

**修复方案**：检查头文件是否提供 `get<T>(key, default_value)` 重载。若不存在，添加：

```cpp
template <typename T>
T get(const std::string& key, const T& default_value) const {
    auto* val = find(key);
    if (!val) return default_value;
    return *val;
}

// 或 get_or
template <typename T>
T get_or(const std::string& key, const T& default_value) const {
    // ...
}
```

**风险**：需确认 ConfigManager 现有 API；**缓解**：先 Grep 现有 `get`/`get_or` 调用确认 API。

---

### F.5 app_config 默认路径回退（8.21，P2）

**问题**：[app_config.cpp:159-173](file:///c:\Users\young\Desktop\Develop\WorkX\src\app\config\app_config.cpp#L159) 无 `APPDATA` 环境变量时回退当前目录，从快捷方式或不同工作目录启动时配置文件丢失。

**修复方案**：优先级链：

```cpp
fs::path get_config_dir() {
    // 1. $WORKX_CONFIG_DIR 环境变量
    if (auto* env = std::getenv("WORKX_CONFIG_DIR")) {
        return fs::path(env);
    }
    // 2. $APPDATA (Windows) / $XDG_CONFIG_HOME (POSIX)
#ifdef _WIN32
    if (auto* env = std::getenv("APPDATA")) {
        return fs::path(env) / "workx";
    }
    // 3. $HOME (POSIX fallback)
    if (auto* env = std::getenv("USERPROFILE")) {
        return fs::path(env) / ".workx";
    }
#else
    if (auto* env = std::getenv("XDG_CONFIG_HOME")) {
        return fs::path(env) / "workx";
    }
    if (auto* env = std::getenv("HOME")) {
        return fs::path(env) / ".config" / "workx";
    }
#endif
    // 4. 最后回退当前目录
    return fs::current_path() / ".workx";
}
```

**风险**：无；改善默认行为。

---

### F.6 CommandPanel/FileSearchPanel 硬编码布局（8.22，P2）

**问题**：[command_panel.cpp:107,145](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\widgets\command_panel.cpp#L107)、[file_search_panel.cpp:107-111](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\widgets\file_search_panel.cpp#L107) `clear_start = height - 1 - MAX_DISPLAY` 可能为负；`padding = 16 - ...` 硬编码 16。

**修复方案**：

```cpp
// 用 std::max 保护负值
int clear_start = std::max(0, height - 1 - MAX_DISPLAY);

// padding 动态计算
int padding = std::min(16, std::max(4, width / 8));
```

**风险**：无；防御性编程。

---

### F.7 SelectPanel 面板超出屏幕（8.23，P2）

**问题**：[select_panel.cpp:147](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\widgets\select_panel.cpp#L147) `start_row = term_h - panel_h - 3` 未处理 `panel_h > term_h`，小屏幕上 start_row 为负数。

**修复方案**：

```cpp
int start_row;
if (panel_h + 3 > term_h) {
    // 面板超出屏幕，从顶部开始，并裁剪 panel_h
    start_row = 0;
    panel_h = std::max(1, term_h - 3);
} else {
    start_row = term_h - panel_h - 3;
}
```

**风险**：无；防御性编程。

---

### F.8 m_total_tokens 负数保护（8.24，P3）

**问题**：[chat_renderer.cpp:451](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\render\chat_renderer.cpp#L451) 未检查 `e.token_count` 是否为负。

**修复方案**：

```cpp
if (e.token_count > 0) {
    m_total_tokens += e.token_count;
}
```

**风险**：无。

---

### F.9 ProviderPreset 模型版本从配置加载（5.13，P3）

**问题**：[model/provider_preset.cpp:19,27](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\model\provider_preset.cpp#L19) OpenAI `gpt-4o`、Anthropic `claude-sonnet-4-20250514` 已过时（当前 2026 年 7 月）。

**修复方案**：**保守方案**：更新硬编码到最新版本，并在注释中标记更新日期。从配置加载需要 ConfigManager 集成，工作量大，留待后续。

```cpp
// 更新预设
{"openai", ProviderType::OpenAI, "https://api.openai.com/v1",
    "gpt-4o-2024-11-20"},  // 2025-01 更新
{"anthropic", ProviderType::Anthropic, "https://api.anthropic.com",
    "claude-sonnet-4-5-20250929"},  // 2025-09 更新
// ...
```

**风险**：版本号会再次过时；**缓解**：注释标记日期，定期更新。

---

### F.10 find_preset 线性搜索（5.14，P3）

**问题**：[model/provider_preset.cpp:74-78](file:///c:\Users\young\Desktop\Develop\WorkX\src\agent\model\provider_preset.cpp#L74) 线性搜索。

**修复方案**：**保留现状**。预设仅 7 个，线性搜索 O(7) 影响可忽略。改 `unordered_map` 需初始化时构建，复杂度增加收益微小。记录决策。

---

## Part G. 平台与命名（4 项）

### G.1 Win32 SHORT vs int 比较（7.8，P2）

**问题**：[platform_win32.cpp:194](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\core\platform\platform_win32.cpp#L194) `initial_pos.X`（SHORT 16 位）与 `viewport_width - 1`（int）比较，`viewport_width - 1 > 32767` 时结果错误（罕见）。

**修复方案**：cast 到 int 后比较，并加范围检查：

```cpp
int pos_x = static_cast<int>(initial_pos.X);
int max_x = std::min(static_cast<int>(viewport_width - 1), 32767);
if (pos_x > max_x) { ... }
```

**风险**：无；防御性编程。

---

### G.2 Win32 flush 在 VT 模式下 no-op（7.9，P2）

**问题**：[platform_win32.cpp:174-178](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\core\platform\platform_win32.cpp#L174) `fflush(stdout)` 在 VT 模式下是 no-op，混用 `printf` 和 `WriteConsole` 时缓冲区未刷新导致输出顺序错乱。

**修复方案**：用 `FlushFileBuffers` 或显式 `WriteConsole` 替代 `printf`：

```cpp
void flush() override {
    if (m_vt_mode) {
        // VT 模式下用 WriteConsole 刷新
        // 或调用 FlushFileBuffers(m_stdout_handle)
        FlushFileBuffers(m_stdout_handle);
    } else {
        fflush(stdout);
    }
}
```

**风险**：无。

---

### G.3 Terminal::initialize USERPROFILE（7.10，P3）

**问题**：[terminal.cpp:62-66](file:///c:\Users\young\Desktop\Develop\WorkX\src\tui\core\terminal.cpp#L62) POSIX 系统应使用 `HOME`（虽有 `#ifdef _WIN32` 保护）。

**修复方案**：检查现有 `#ifdef _WIN32` 保护是否完整：

```cpp
std::string get_home_dir() {
#ifdef _WIN32
    if (auto* env = std::getenv("USERPROFILE")) return env;
    if (auto* drive = std::getenv("HOMEDRIVE")) {
        if (auto* path = std::getenv("HOMEPATH")) {
            return std::string(drive) + path;
        }
    }
    return ".";
#else
    if (auto* env = std::getenv("HOME")) return env;
    return "~";
#endif
}
```

**风险**：无；改善跨平台兼容性。

---

### G.4 command/source/ 目录风格（1.5 子项，P3）

**问题**：`agent/command/source/` 与项目其他模块（.h/.cpp 同级）风格不一致。

**修复方案**：**保留现状**。目录重组涉及大量 include 修改，本次仅记录决策，留待后续重构。

---

## 实施批次

### 批次 1：纯清理（低风险，无依赖）
1. A.1 namespace 注释批量替换
2. A.7 i_backend.h 空行清理
3. A.9 constants.h 死代码删除
4. D.6 API Key 明文（仅文档记录）
5. E.1 `**bold**` 改粗体
6. E.3 bullet `+` 列表
7. E.7 LineEditor::read_line 重置历史
8. E.11 Terminal::end_overlay DECSC/DECRC
9. E.12 is_list_item dot > 0 修复
10. F.8 m_total_tokens 负数保护
11. F.10 find_preset 线性搜索（保留，仅记录决策）

### 批次 2：代码组织抽取（中风险，需 Grep 验证）
12. A.3 include 风格统一
13. A.4 BOM 跳过抽取
14. A.5 LF 规范化抽取
15. A.6 UTF-16 LE/BE 模板化
16. A.8 ExecutorResult 死代码（加 TODO 注释）
17. A.11 `#ifdef WORKX_HAS_NLOHMANN_JSON` 统一
18. D.3 SSE `\r\n\r\n` 分隔符修复
19. D.8 provider_type_from_string 大小写

### 批次 3：API 与适配器重构（中风险，接口变更）
20. D.1 SharedPtrWrapper 删除（接口变更）
21. D.2 SSEEvent::has_retry 改 optional
22. D.4 schema additionalProperties
23. D.7 BackendConfig api_key 校验
24. D.9 build_preset_url 改 optional
25. D.10 ProviderPreset string_view → string
26. F.1 PromptBlock::type enum class
27. F.2 CommandResult::Compact 加 TODO
28. F.3 CommandBase setter 加锁

### 批次 4：函数拆分（高风险，需充分测试）
29. B.1 run_completion 拆分
30. B.2 重复代码抽取
31. B.3 run_stream 拆分
32. B.4 session_id 参数化

### 批次 5：性能优化（高风险，需基准测试）
33. C.1 LCS hash 预处理
34. C.2 GlobTool 手写 matcher
35. C.3 poll 线程 curl_multi_wait
36. C.4 sessions_by_reader 清理
37. C.5 StreamSession::cancel 立即取消
38. C.6 StreamSession 析构悬空修复
39. C.7 get() 连接超时
40. C.8 SSEStreamReader buffer 删除
41. C.9 SSEParser erase
42. C.10 SSE chunk string_view
43. C.11 TaskManager condition_variable
44. C.12 StatusBar dirty 优化
45. C.13 buf(code) 拷贝优化

### 批次 6：渲染质量改进（中风险）
46. E.2 TableBuffer::feed_line Invalid
47. E.4 OutputFormatter 表格缩进
48. E.5 syntax_highlighter Diff prefix enum
49. E.6 classify_by_type 精确匹配
50. E.8 LineEditor is_special_char 多字节
51. E.9 ChatRenderer 硬编码列位置
52. E.10 SetupWizard 多字节支持

### 批次 7：配置与平台
53. F.4 ConfigScope::get 重载
54. F.5 app_config 默认路径
55. F.6 CommandPanel 硬编码布局
56. F.7 SelectPanel 面板超出
57. F.9 ProviderPreset 模型版本更新
58. G.1 Win32 SHORT vs int
59. G.2 Win32 flush
60. G.3 Terminal USERPROFILE

### 批次 8：剩余项
61. A.2 移植痕迹清理（仅注释）
62. A.10 command/inclaude 命名（保留，记录决策）
63. G.4 command/source 目录（保留，记录决策）

### 批次 9：验证
64. 编译通过
65. 单元测试（如有）
66. 手动测试：渲染、API、工具调用全链路

---

## 验证清单

阶段 4 完成后需验证：

### 编译
- [ ] 编译无 warning（MSVC /W4）
- [ ] 无链接错误（SharedPtrWrapper 删除后调用方正常）
- [ ] namespace 注释替换后无遗漏

### 代码组织
- [ ] Grep `} // namespace workx` 返回 0 结果
- [ ] Grep `#include "\.\./` 返回 0 结果
- [ ] Grep `WORKX_HAS_NLOHMANN_JSON` 仅在 CMakeLists.txt 出现
- [ ] Grep `对应参考实现` 返回 0 结果

### 性能
- [ ] LCS diff 2 万行文件 < 1 秒
- [ ] GlobTool 大目录（10 万文件）< 2 秒
- [ ] HTTP poll 线程 CPU 占用 < 1%（空闲时）
- [ ] TaskManager waitForAll 不再 busy-wait
- [ ] StatusBar 空闲时不重绘

### 渲染
- [ ] `**bold**` 显示为粗体而非黄色
- [ ] `+ item` 列表正常渲染
- [ ] 嵌套表格渲染正确
- [ ] 中文 API Key 在 SetupWizard 可输入
- [ ] LineEditor 历史浏览重置正确

### API 适配器
- [ ] SharedPtrWrapper 删除后流式调用正常
- [ ] SSEEvent retry=0 不被当作未设置
- [ ] nlohmann_json 强依赖后编译正常

### 配置
- [ ] 无 APPDATA 时配置路径回退 HOME
- [ ] CommandPanel 小屏幕不崩溃
- [ ] SelectPanel 小屏幕不崩溃
- [ ] unknown provider 报错清晰

---

## 待审批事项

请审阅本 Plan 并确认：

1. **范围确认**：阶段 4 仅覆盖 P2/P3，剩余 P1（agent/core 2.6-2.13、agent/tool 3.7-3.20、agent/command 5.1-5.8、tui 6.6-6.22、platform 7.2-7.7、core/app 8.9-8.17）留待阶段 5+？还是希望阶段 4 也包含这些 P1？

2. **批次顺序**：批次 1（纯清理）→ 8（剩余项）的顺序是否合适？是否需要调整？

3. **A.10 / G.4 目录命名**：`inclaude`、`command/source/` 保留现状仅记录决策，是否符合预期？

4. **A.11 nlohmann_json 强依赖**：是否同意删除 `#ifdef WORKX_HAS_NLOHMANN_JSON` 回退分支，强制依赖 nlohmann_json？

5. **D.1 SharedPtrWrapper 删除**：接口变更影响 IBackend 所有实现，是否同意？

6. **D.6 API Key 明文存储**：本次仅文档记录不实现 secure_string，是否同意？

7. **F.2 CommandResult::Compact**：仅加 TODO 注释（依赖 stub 模块），是否同意？

8. **F.10 find_preset 线性搜索**：保留现状仅记录决策，是否同意？

9. **F.9 ProviderPreset 模型版本**：仅更新硬编码到最新版本，不从配置加载，是否同意？

10. **是否进入执行阶段？**

待你审批后，我将按批次顺序执行修改。整个阶段 4 预计涉及 30+ 文件的修改，无新增文件。

依赖关系：
- 阶段 4 批次 4（函数拆分）依赖阶段 1 完成（mutex 已引入）
- 阶段 4 批次 6（E.8 LineEditor 多字节）依赖阶段 2 完成（char32_width 已抽取）
- 阶段 4 批次 5（C.10 SSE string_view）独立
- 其他批次无强依赖
