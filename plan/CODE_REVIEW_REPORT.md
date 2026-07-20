# WorkX 源代码审查报告

> **状态**：待审批（未做任何代码修改）
> **范围**：`src/` 全部子目录（agent / app / core / tui）
> **方法**：5 个并行子代理只读分析 + 跨模块汇总
> **统计**：覆盖 70+ 源文件，发现 **约 130 个问题**，其中 P0 级 17 项、P1 级 35 项、P2 级 40+ 项

---

## 0. 阅读指南

| 严重级别 | 含义 | 处置建议 |
|---------|------|---------|
| **P0** | 会导致崩溃、数据损坏、安全漏洞、违反硬约束 | 必修 |
| **P1** | 严重功能 Bug、并发竞态、API 适配错误、关键 stub 缺失 | 应尽快修 |
| **P2** | 中等设计/性能/可维护性问题 | 计划修复 |
| **P3** | 命名、风格、轻微冗余 | 视情况修 |

---

## 1. 全局性问题

### 1.1【P3】命名空间注释与声明不一致
- **位置**：40+ 文件，如 `core/task/task_manager.h:156`、`core/events/event_bus.h:200`、`app/config/cli_args.cpp:102`、`tui/render/spinner.h:67`
- 实际声明 `namespace agent`，闭合注释全部写为 `} // namespace workx`
- 影响：IDE 折叠/重构失效，可维护性下降
- 修复：批量替换 `} // namespace workx` → `} // namespace agent`

### 1.2【P1】14 个核心模块仅有文档注释，无任何实现
- **位置**：
  - `agent/permission/` 全部（checker.h, mode.h, rule.h）
  - `agent/compact/` 全部（token_count.h, truncator.h）
  - `agent/prompt/` 全部（memory.h, system_prompt.h）
  - `agent/model/config.h`、`model/router.h`
  - `agent/message/builder.h`、`message/history.h`
  - `agent/util/` 全部（async.h, json_schema.h, string_util.h）
  - `agent/api/retry.h`
- 影响：权限校验、Token 压缩、系统提示构建、JSON Schema 校验、异步工具等关键功能完全缺失。对一个会执行 Shell 与文件写操作的 Code Agent 而言，**权限缺失是安全底线问题**。
- 建议：分阶段落地，permission + json_schema + compact 优先

### 1.3【P2】移植痕迹明显，文件名与目录职责错位
- `agent/message/types.h:1-2` 文件头注释为 `@file events.h`，实际定义的是事件类型
- `agent/message/types.h` 含 `UserInputEvent`/`StreamTokenEvent`/`ToolCallEvent`，应归 `events/`
- `agent/message/types.h:79-87` 定义 `ToolType` 枚举，应归 `tool/types.h`
- 多处出现 "对应参考实现中的 xxx.ts" 注释，疑似从 Claude Code CLI 移植

### 1.4【P2】include 风格不统一
- 同一模块内混合使用 `"../inclaude/executor.h"` 相对路径与 `"agent/command/inclaude/registry.h"` 项目根路径
- 建议统一为项目根路径

### 1.5【P3】command 子目录命名混乱
- `agent/command/inclaude/` 含义不清（疑似 "include" 笔误或 "in-claude" 缩写）
- `agent/command/source/` 与项目其他模块（.h/.cpp 同级）风格不一致

---

## 2. agent/core 模块（chat_session、input、query_engine、react_loop）

### 2.1【P0】ChatSession 多线程无同步，数据竞争
- **位置**：`agent/core/chat_session.cpp` 全文
- `m_messages`、`m_system_prompt`、`m_tool_registry` 在主线程读写，同时被 `run_completion` 启动的后台 Task 大量修改，**无任何 mutex 保护**
- 调用路径：
  - 主线程：`send_message`/`clear_history`/`regenerate`/`save_session`/`load_session`/`set_system_prompt`/`set_tool_registry`
  - 后台 Task：`run_completion` 内大量 `push_back`/`pop_back`/读取
- 影响：用户在生成中执行 `/clear`、`/regen` 或保存会话即触发 UB

### 2.2【P0】后台 lambda 捕获 this，析构不等待 Task 完成
- **位置**：`chat_session.cpp:122-124`（捕获 this）+ `chat_session.cpp:36-41`（析构）
- `~ChatSession` 只调用 `m_provider->interrupt()`，未等待后台 Task 结束
- 影响：生成中析构会 use-after-free

### 2.3【P0】agent 循环 lambda 无 try-catch 包裹
- **位置**：`chat_session.cpp:122-423`
- `m_provider->submit_completion`、`m_tool_executor->execute`、`std::filesystem::current_path` 均可能抛异常
- 任何异常逃逸 → `m_generating` 永不复位 → 会话永久不可用

### 2.4【P0】`regenerate()` 未检查 `m_generating`
- **位置**：`chat_session.cpp:80-91`
- `send_message` 有检查，但 `regenerate()` 直接启动新 Task，可与正在运行的 Task 并发

### 2.5【P1】递归重试时 `m_generating` 被错误重置
- **位置**：`chat_session.cpp:138-170`、`263-300`
- 重试在 lambda 内启动新 Task 后立即 `m_generating.store(false)`，新 Task 还未真正开始
- 影响：重试窗口内可提交新消息，造成并发 Task

### 2.6【P1】达到 max_iterations 时丢失最后一轮输出
- **位置**：`chat_session.cpp:126, 308-326, 414-420`
- `max_iterations = 25` 硬编码且不可配置
- 退出时未保存最后一轮 `full_content` 和 `pending_tools`，用户看不到部分回答

### 2.7【P1】工具执行结果未区分错误
- **位置**：`chat_session.cpp:394-408`
- `exec_result.is_error` 字段被忽略，LLM 无法区分工具失败/成功

### 2.8【P1】tool input JSON 解析失败被静默替换为空对象
- **位置**：`chat_session.cpp:338-344, 376-383`
- 工具以空参数执行，可能产生意外行为（如 FileReadTool 收到空 path）

### 2.9【P1】`process_bash_command` 完全丢弃输出
- **位置**：`agent/input/processor.h:60-64`
  ```cpp
  ProcessResult process_bash_command(const std::string& command) {
      std::string output = execute_bash(command);
      return { };  // output 被丢弃
  }
  ```
- 影响：`!ls` 等命令无任何输出，功能完全不可用

### 2.10【P1】`process_text_prompt` 完全忽略 `image_paths`
- **位置**：`agent/input/processor.h:66-84`
- 图片附件不传递给 LLM，多模态输入功能缺失

### 2.11【P1】`process_slash_command` 拼接产生尾随空格
- **位置**：`agent/input/processor.h:51`
- `cmd.args` 为空时生成 `"/cmd "`，下游解析可能误判

### 2.12【P1】`InputProcessor::process` 无 try-catch
- **位置**：`processor.h:24-45`
- 调用 `execute_bash`/`read_file_content`/`execute`，任一抛异常逃逸到上游

### 2.13【P1】`read_file_content` 是 stub
- **位置**：`processor.h:91-94`
- 返回 `"[file content: " + path + "]"` 而非真实内容，文件附件功能不可用

### 2.14【P2】`run_completion` 函数过长（~320 行）
- **位置**：`chat_session.cpp:106-424`
- 单函数承担 agent 循环、流式读取、取消处理、重试、工具执行、消息构建
- 应拆分为 `run_agent_loop`/`read_stream_response`/`execute_tool_uses`/`wait_with_cancel`

### 2.15【P2】大量重复代码
- backoff 等待：`chat_session.cpp:147-156` 与 `273-281` 几乎相同
- StreamDoneEvent 发布：`314-323` 与 `350-359` 几乎相同
- tool input JSON 解析：`338-344` 与 `376-383` 完全相同

### 2.16【P2】硬编码 session_id `"default"`
- **位置**：`chat_session.cpp` 11+ 处
- 无法支持多会话

### 2.17【P2】`build_request` 每轮迭代完整拷贝 `m_messages`
- **位置**：`chat_session.cpp:64-66`
- 长对话 + 多轮工具调用时 O(n²) 拷贝

### 2.18【P2】parser.h 引号处理不完整
- **位置**：`agent/input/parser.h:92-117`
- 不支持转义引号、引号不配对时 `in_quote` 永真

### 2.19【P2】`ParsedInput` 应使用 `std::variant`
- **位置**：`agent/input/types.h:28-34`
- 当前枚举 + 多个可选字段，可在编译期保证类型安全

### 2.20【P2】`query_engine.h`/`react_loop.h` 是空 stub
- 仅注释，无实现，但描述的职责与 `ChatSession::run_completion` 严重重叠
- 架构混乱，读者困惑：agent 循环到底由谁负责？

---

## 3. agent/tool 模块（工具集）

### 3.1【P0】ToolExecutor::execute 无 try-catch
- **位置**：`agent/tool/executor.h:87`
- 任何工具抛异常（bad_alloc、filesystem_error、json::exception）→ 整个 Agent 崩溃

### 3.2【P0】FileWriteTool 写入失败导致数据丢失
- **位置**：`agent/tool/FileWriteTool/file_write_tool.cpp:321-336`
- `ios::trunc` 先截断再写入，写入失败时文件已损坏，仅靠 `.bak` 备份
- 正确做法：写临时文件 → fsync → 原子 rename

### 3.3【P0】LCS diff 算法 O(n*m) 内存，2MB 文件可 OOM 2.5GB
- **位置**：`agent/tool/FileWriteTool/diff.cpp:60-77`
- `vector<vector<int>> dp(m+1, vector<int>(n+1))`
- 2MB ASCII 约 2.5 万行，矩阵 6.25e8 × 4 字节 ≈ 2.5GB
- 应改用 Myers O(ND) 或滚动数组

### 3.4【P0】`FileReadTool/GlobTool::call` 中 `input.get<T>()` 无 try-catch
- **位置**：`FileReadTool/file_read_tool.cpp:213`、`GlobTool/glob_tool.cpp:128`
- LLM 传类型不匹配字段会抛 `json::type_error`，未被 `validate_input` 拦截（未校验所有字段类型）

### 3.5【P0】配置值绕过文件大小限制
- **位置**：`FileReadTool/file_read_tool.cpp:217-226`
- 配置负数 → `static_cast<size_t>(-1) = SIZE_MAX`，绕过 2MB 限制
- `max_lines=INT_MAX` → `reserve(INT_MAX)` 触发 bad_alloc

### 3.6【P0】`fs::exists` 失败绕过 pre-read 检查（安全）
- **位置**：`FileWriteTool/file_write_tool.cpp:288`
- `ec` 设置时返回 false → 走 create 分支跳过 staleness 检查 → 直接覆盖现有文件

### 3.7【P1】`FileReadTool::call` 文件大小检查可被 ec 绕过
- **位置**：`FileReadTool/file_read_tool.cpp:257-263`
- `ec` 设置时跳过 size 检查，可能继续读超大文件 OOM

### 3.8【P1】`offset - 1 + limit` 整数溢出
- **位置**：`FileReadTool/file_read_tool.cpp:324, 304`
- `validate_input` 只校验 `>=1` 无上限，`offset=INT_MAX, limit=2000` 溢出为负

### 3.9【P1】GlobTool `glob_to_regex` 未转义 `[`、`]`、`{`、`}`
- **位置**：`GlobTool/glob_tool.cpp:105-106`
- glob pattern 含字面 `[`（如 `array[0].cpp`）会触发 `std::regex_error`

### 3.10【P1】GlobTool 无结果数量/深度/.gitignore 限制
- **位置**：`GlobTool/glob_tool.cpp:164-196`
- 无 `max_depth` → 深目录树栈溢出
- 无 `max_results` → 宽 pattern 返回数万条
- 无 `.gitignore` → 遍历 `.git/`、`node_modules/`、`build/`

### 3.11【P1】FileReadTool::read_directory 无数量限制
- **位置**：`FileReadTool/file_read_tool.cpp:164-202`
- 目录含 10 万文件时结果字符串过长

### 3.12【P1】FileWriteTool 路径无白名单
- **位置**：`FileWriteTool/file_write_tool.cpp:243-370`
- 可写任意路径（`/etc/passwd`、`C:\Windows\System32\`），仅受进程权限限制
- 无工作区边界检查

### 3.13【P1】registry 不检查重复注册
- **位置**：`agent/tool/registry.h:30-34`
- 同名注册时 `name_index_` 覆盖但 `tools_` 保留旧指针
- `get_all_schemas()` 返回重复 schema，LLM 困惑

### 3.14【P1】`ToolContext::cancelled_` 用 memory_order_relaxed
- **位置**：`agent/tool/context.h:34, 39`
- 不保证跨线程立即可见，多核 CPU 上有数毫秒延迟
- 应改 acquire/release 或 seq_cst

### 3.15【P1】FileReadStateTracker 锁内拷贝大字符串
- **位置**：`agent/tool/FileReadState/file_read_state.cpp:32-41`
- `get_state` 持锁拷贝整个 `FileReadState`（含可能 2MB 内容）
- 应返回 `shared_ptr<const FileReadState>` 或用 `shared_mutex`

### 3.16【P1】FileReadStateTracker 单例无 LRU 淘汰
- **位置**：`agent/tool/FileReadState/file_read_state.h:51-118`
- 长期运行累积读取状态，内存只增不减

### 3.17【P1】encoding.cpp `gbk_to_utf8` 返回值未检查
- **位置**：`agent/tool/encoding.cpp:122-135`
- 第二次 `MultiByteToWideChar`/`WideCharToMultiByte` 失败时返回空串/乱码，不报错
- `size > INT_MAX` 时 `static_cast<int>(size)` 截断

### 3.18【P1】diff hunk 合并阈值偏差
- **位置**：`FileWriteTool/diff.cpp:224`
- `> 2*context_lines + 1` 比标准多 1，与 git diff 输出不一致

### 3.19【P1】CRLF 行尾差异不显示
- **位置**：`FileWriteTool/diff.cpp:25-52`
- `split_lines` 移除 `\r`，原 CRLF、新 LF 时 diff 不显示行尾变化
- LLM 看到的 diff 与实际文件变化不一致，可能反复"修复"行尾

### 3.20【P1】BashTool/GrepTool/AgentTool/MCPTool/WebFetchTool/FileEditTool 全部 stub
- **位置**：`agent/tool/` 各子目录
- `input_schema` 已声明，LLM 会调用但得到 "not implemented" 错误，浪费 LLM token
- 应在 LLM prompt 中标注未实现，或不注册

### 3.21【P2】schema 缺 `additionalProperties: false`
- `FileEditTool`、`GlobTool`、`GrepTool`、`BashTool`、`AgentTool`、`MCPTool`、`WebFetchTool` 全部缺失
- `FileReadTool`/`FileWriteTool` 已对齐，其他不一致

### 3.22【P2】LCS 性能 O(n*m) 时间
- **位置**：`diff.cpp:60-77`
- 2 万行文件需 4 亿次比较，秒级延迟
- 改 Myers O(ND) 或对相同行做 hash 预处理

### 3.23【P2】GlobTool `std::regex` 性能差
- **位置**：`GlobTool/glob_tool.cpp:149-188`
- 大目录（10 万文件）耗时秒级
- 应改手写 glob matcher

### 3.24【P2】UTF-16 LE/BE 转换代码重复
- **位置**：`encoding.cpp:32-116`
- `utf16le_to_utf8` 和 `utf16be_to_utf8` 几乎完全相同，仅字节序不同
- 应模板化或合并

### 3.25【P2】BOM 跳过逻辑重复
- **位置**：`FileReadTool/file_read_tool.cpp:287-296` 与 `encoding.cpp:288-297`
- 两处完全相同，应抽取到 `encoding.h`

### 3.26【P2】LF 规范化逻辑重复
- **位置**：`FileWriteTool/file_write_tool.cpp:45-97`
- `read_file_lf_normalized` 与 `lf_normalize` 共享相同逻辑，应抽取

### 3.27【P2】`FileReadTool::call` 中 `lines_read` 变量遮蔽
- **位置**：`FileReadTool/file_read_tool.cpp:367, 382`
- 内层与外层重复定义同名变量

### 3.28【P2】encoding.cpp `Unknown` 编码回退 UTF-8
- **位置**：`encoding.cpp:282-283`
- Unknown 走 UTF-8 路径可能产生乱码，应警告或拒绝

### 3.29【P3】constants.h 死代码
- `PDF_MAX_PAGES_PER_READ = 20` 定义但 FileReadTool 未实现 PDF 读取

---

## 4. agent/api 模块（LLM 后端接口）

### 4.1【P0】`chat_async` 双重包装回调导致 assistant 消息重复入历史
- **位置**：`agent/api/client.cpp:406-436` 与 `438-494`
- `chat_async` 已包装 `on_token`/`on_done`，`stream_chat_async` 又包装一次
- `on_done` 触发两次 → `m_messages.push_back(assistant)` 被调用两次 → 历史里两条相同消息

### 4.2【P0】重试时未清空已累积输出，内容拼接错乱
- **位置**：`client.cpp:188-338`（`run_stream`）
- 重试时 `content_out`/`reasoning_out` 未 clear，新内容拼在旧内容后

### 4.3【P0】`m_messages` 完全无线程同步
- **位置**：`client.cpp` 全文 + `client.h:160`
- 异步任务后台 `push_back`，主线程同时 `history()`/`clear_history()` → 数据竞争

### 4.4【P0】`stream_chat_async` 的 generating 检查存在 TOCTOU
- **位置**：`client.cpp:440-447`
- `load()` 与 `store()` 不构成原子 CAS，两个线程可同时通过检查
- 应用 `compare_exchange_strong` 或 mutex

### 4.5【P0】`stream_chat_async` 捕获 this，Client 析构后悬空
- **位置**：`client.cpp:481-491`
- `~Client` 调 `m_backend->shutdown()`，但 TaskManager 任务仍可能跑并访问已释放成员
- 没有等待任务完成的逻辑

### 4.6【P0】HTTP 总超时被错误用于流式传输
- **位置**：`agent/api/remote/http_client.cpp:162`
- `CURLOPT_TIMEOUT_MS` 是整个传输总时长，30000ms 超时会让长响应被中途切断
- 应改用 `CURLOPT_LOW_SPEED_LIMIT` + `CURLOPT_LOW_SPEED_TIME` 空闲超时

### 4.7【P0】SSE parser 把全部响应内容记入 INFO 日志
- **位置**：`agent/api/sse_parser.cpp:117`
- `LOG_INFO("SSEParser parsed event: {}", event.data)`
- 包含模型回复、tool_use 参数（含代码、用户隐私）、reasoning
- 生产环境泄露敏感信息，应改 DEBUG 或只打印长度/哈希

### 4.8【P0】`list_models` 日志泄露 Anthropic `x-api-key`
- **位置**：`agent/api/remote/remote_backend.cpp:175-178`
- 只对 `Authorization` 头脱敏，未对 `x-api-key` 脱敏
- Anthropic provider 调用时 API Key 完整打印到日志

### 4.9【P1】`chat` 失败时历史已污染
- **位置**：`client.cpp:344-358, 374-388`
- `chat(user_text)` 在 `run_stream` **之前**就 push user 消息
- 失败时孤儿消息留在历史，下次 chat 上下文错位
- 而 `chat(const std::vector<ChatMessage>&)` 又不 push，行为不一致

### 4.10【P1】`cancel_stream(const SSEStreamReader*) const` 声明未实现
- **位置**：`http_client.h:50`
- 头文件声明，cpp 中无实现，调用即链接错误

### 4.11【P1】OpenAI 流式 usage 永远为 0
- **位置**：`provider/openai_adapter.cpp:182-187`
- OpenAI 流式默认不返回 usage，必须设 `stream_options: {"include_usage": true}`
- 全工程未出现 `stream_options`/`include_usage`

### 4.12【P1】Anthropic `list_models` 端点错误
- **位置**：`remote_backend.cpp:162-168`
- 无论 provider 都硬编码 `/v1/models`，Anthropic 无此端点 → 必然 404

### 4.13【P1】OpenAI `parse_sse_event` 仅处理第一个 tool_call
- **位置**：`provider/openai_adapter.cpp:152-175`
- `delta["tool_calls"][0]` 只取第一个，丢弃 index > 0 的所有 tool_call

### 4.14【P1】OpenAI finish_reason 缺 `content_filter`
- **位置**：`openai_adapter.cpp:180`
- 缺 `content_filter` 处理 → 流会卡住直到超时

### 4.15【P1】Anthropic 用 `operator[]` 访问可能不存在的键
- **位置**：`provider/anthropic_adapter.cpp:152, 166, 182`
- 缺字段时抛 `out_of_range`，虽 catch 能捕获但反模式，应 `.value()`/`.contains()`

### 4.16【P1】Anthropic assistant 的 reasoning_content 被拼进 content
- **位置**：`anthropic_adapter.cpp:89-93`
- 应结构化输出 `thinking` content block
- 当前拼成一个 text block，模型无法区分思考与回复，多轮对话严重劣化

### 4.17【P1】Anthropic 多个 tool_result 被拆成多条 user 消息
- **位置**：`anthropic_adapter.cpp:99-110`
- Anthropic 规范要求连续 tool_result 必须放同一 user 消息的 content 数组
- 当前实现导致 API 报 400 或模型行为异常

### 4.18【P1】OpenAI assistant with tool_calls 时 content 字段处理不当
- **位置**：`openai_adapter.cpp:54`
- 强制 `m["content"] = ""`，应为 `null`（部分兼容 API 报错）

### 4.19【P1】OpenAI Tool 消息缺少 tool_call_id 校验
- **位置**：`openai_adapter.cpp:58-60`
- 空时仍写入空字符串 → OpenAI 返回 400

### 4.20【P1】Anthropic system_prompt 多条只取最后一条
- **位置**：`anthropic_adapter.cpp:56-60`
- 多条 system 时后者覆盖前者，应拼接

### 4.21【P1】Anthropic `message_delta` 不解析 `input_tokens`
- **位置**：`anthropic_adapter.cpp:189-192`
- 只取 `output_tokens`，`prompt_tokens` 永远是 0

### 4.22【P1】`async_post_stream` 提交失败时状态不一致
- **位置**：`http_client.cpp:320`
- curl 初始化失败时直接 return，但调用方已把 reader 存入 `m_active_reader`
- reader 永远不会收到数据，`next()` 无限阻塞

### 4.23【P1】HTTP 错误响应体被丢弃
- **位置**：`http_client.cpp:198-200`
- 只传状态码字符串，丢弃 JSON 错误体
- 上层只看到 "HTTP error: 429" 看不到 "rate_limit_exceeded, retry after 30s"

### 4.24【P1】OpenAI 流式 error 事件被静默吞掉
- **位置**：`openai_adapter.cpp:129-131`
- `return false` 不设 is_final、不填错误信息，流继续等待最终超时

### 4.25【P1】`RemoteBackend::m_retry_count`/`m_retry_delay_ms` 死代码
- **位置**：`remote_backend.h:66-67`、`remote_backend.cpp:47-48`
- 从 ConfigManager 读取但从不使用，重试实际在 Client 里用 Client 自己的字段
- 配置项 `backend.retry_count`/`backend.retry_delay_ms` 看似生效实则无效

### 4.26【P1】`RemoteBackend::submit_completion` 不检查是否已有在飞请求
- **位置**：`remote_backend.cpp:102-139`
- 直接覆盖 `m_active_reader`，旧 reader 被覆盖但 HTTP 仍跑
- `m_active_reader` 无锁保护，与 `interrupt` 并发竞态

### 4.27【P1】Client::regenerate 不检查 generating
- **位置**：`client.cpp:508-513`
- 生成中调用会与后台 push_back 竞态

### 4.28【P1】重试退避 `1 << attempt` 溢出风险
- **位置**：`client.cpp:197, 308`
- `attempt=30` 是 UB，配置 10+ 时 int 溢出
- 应用 64 位或加 cap

### 4.29【P2】SSE `\r\n\r\n` 分隔符截取长度不严谨
- **位置**：`sse_parser.cpp:53`
- 无论分隔符长度都固定减 2，对 `\r\n\r\n` 保留 2 字节分隔符在末尾

### 4.30【P2】poll 线程忙等
- **位置**：`http_client.cpp:277-281`
- 无传输时每 10ms 轮询 `curl_multi_perform`，长期空转浪费 CPU
- 应改条件变量唤醒

### 4.31【P2】`sessions_by_reader` 内存泄漏
- **位置**：`http_client.cpp:267-270`
- reader 销毁但 session 未完成时 map 条目永不清理

### 4.32【P2】`StreamSession::cancel` 不是立即取消
- **位置**：`http_client.cpp:189`
- 只设置原子标志，依赖下一次 write 回调返回 0
- 应同时 `curl_multi_remove_handle`

### 4.33【P2】`StreamSession` 析构时 `m_multi` 可能悬空
- **位置**：`http_client.cpp:174-180`
- shutdown 被调两次时 multi 已 nullptr 但 `m_added_to_multi` 仍为 true → segfault

### 4.34【P2】`get()` 无连接超时、无重试、无 keepalive
- **位置**：`http_client.cpp:93-127`
- 只设总超时未设连接超时，TCP 不响应时整 timeout 花在连接阶段
- 每次 `get` 都 `curl_easy_init`/`cleanup`，不复用连接

### 4.35【P2】OpenAI 空 choices 的 usage chunk 被丢
- **位置**：`openai_adapter.cpp:134-136`
- 开启 `include_usage` 后最后 chunk 的 choices 为空，usage 有值，但代码直接 return false

### 4.36【P2】`SharedPtrWrapper` 设计冗余
- **位置**：`remote_backend.h:23-31`
- 为返回 `unique_ptr<IStreamReader>` 而内部用 `shared_ptr` 搞了个 wrapper
- 应让 `submit_completion` 直接返回 `shared_ptr`

### 4.37【P2】`run_stream` 函数过长（160 行）
- **位置**：`client.cpp:178-338`
- 应拆分为 `handle_submit_failure`/`handle_stream_error`/`wait_with_interrupt`

### 4.38【P2】两处重复的退避等待代码
- **位置**：`client.cpp:212-219` 与 `312-319`
- 完全相同的可中断睡眠，应抽 `interruptible_sleep`（`retry.h` 本该承担，但只有注释）

### 4.39【P2】Anthropic/OpenAI 适配器 URL 尾部斜杠清理重复
- **位置**：`anthropic_adapter.cpp:22-25`、`openai_adapter.cpp:18-22`
- 应抽到基类或工具函数

### 4.40【P2】`#ifdef WORKX_HAS_NLOHMANN_JSON` 散落各处
- **位置**：`anthropic_adapter.cpp`/`openai_adapter.cpp`/`remote_backend.cpp` 多处
- 回退路径返回残缺 JSON（空 messages 数组会被 API 拒绝）
- 应编译期统一决定，无 JSON 库就直接编译报错

### 4.41【P2】SSEStreamReader `m_content_buffer`/`m_reasoning_buffer` 累积但未使用
- **位置**：`sse_stream_reader.cpp:97-103`、`sse_stream_reader.h:73-74`
- 长响应下无限增长占内存，从不被读取

### 4.42【P2】SSEParser 用 `substr` 反复重建缓冲区
- **位置**：`sse_parser.cpp:71-73`
- O(n) 拷贝，应改 `erase(0, last_event_end)` 或环形缓冲

### 4.43【P2】每个 SSE chunk 都构造 `std::string` 拷贝
- **位置**：`http_client.cpp:218`
- 高频流式下拷贝开销显著，`feed_data` 应改 `string_view` 或 `const char*+size`

### 4.44【P2】SSEEvent::has_retry 判定可疑
- **位置**：`sse_parser.hpp:26`
- `retry > 0` 把合法的 `retry: 0`（立即重连）当作未设置

### 4.45【P2】`i_backend.h` 默认实现里有莫名空行
- **位置**：`i_backend.h:43-48`
- 三行空行，编辑残留

### 4.46【P3】API Key 以明文存储
- **位置**：`chat_types.h:136`
- `std::string` 析构不擦除内存，可能残留堆中

### 4.47【P3】`BackendConfig` 可默认构造、api_key 无校验
- **位置**：`chat_types.h:125-144`
- 空 api_key 导致 401 但错误信息不直观

### 4.48【P3】provider_type_from_string 大小写不敏感实现不完整
- **位置**：`model/provider_type.h:34-44`
- 只支持 `"openai"`/`"OpenAI"`/`"OPENAI"` 三种精确匹配，`"OpenAi"` 等会失败

### 4.49【P3】`build_preset_url` 错误处理糟糕
- **位置**：`model/provider_preset.cpp:88-104`
- 失败返回字符串 `"(custom URL required)"`，调用方难区分错误消息与合法 URL
- 应返回 `Result<std::string, std::string>` 或 `std::optional<std::string>`

### 4.50【P3】ProviderPreset 用 string_view 存在悬空引用风险
- **位置**：`model/provider_preset.h:22-27`
- 用户从临时 `std::string` 构造时 string_view 指向已销毁对象

---

## 5. agent/command 模块（命令系统）

### 5.1【P1】CommandExecutor::execute 使用 dynamic_cast 反模式
- **位置**：`agent/command/source/executor.cpp:39-52`
- 用 `dynamic_cast<LocalCommand*>`/`dynamic_cast<PromptCommand*>` 类型分发
- 违反开闭原则，新增命令类型需改此处
- 应在 `CommandBase` 提供虚函数 `execute()` 多态实现

### 5.2【P1】敏感命令脱敏未实现
- **位置**：`source/executor.cpp:34`
- `CommandBase::is_sensitive()` 标记被完全忽略

### 5.3【P1】未检查 is_model_invocation_disabled
- **位置**：`source/executor.cpp:16-59`
- `CommandBase` 有此接口但 executor 未检查
- 模型可调用被禁止的命令

### 5.4【P1】CommandExecutor::parse 不处理前导空格与多空格
- **位置**：`source/executor.cpp:61-79`
- `  /help` 前导空格时 `input[0] == '/'` 不成立
- `/help   arg` 多空格返回 `args = "  arg"`（含前导空格）

### 5.5【P1】CommandExecutor::parse 不支持引号参数
- **位置**：`source/executor.cpp:71-77`
- `/init "project name with spaces"` 在第一个空格处被切断

### 5.6【P1】CommandRegistry::register_command 未处理重复注册
- **位置**：`source/registry.cpp:15-18`
- `name_index_` 覆盖但 `commands_` 保留旧条目
- `get_user_invocable_commands()`/`get_by_type()` 返回重复/僵尸条目

### 5.7【P1】CommandRegistry::register_command 未校验 nullptr
- **位置**：`source/registry.cpp:15-18`
- 传 nullptr 时 `cmd->name()` 解引用空指针崩溃

### 5.8【P1】PromptCommand::generate_prompt 未设置 generator 时产生空查询
- **位置**：`inclaude/command.h:128-131` + `source/executor.cpp:42-49`
- 未设置时返回空 vector，executor 拼接空字符串 → `should_query=true`
- 向模型发送空 prompt，浪费 API 调用

### 5.9【P2】PromptBlock::type 使用字符串而非枚举
- **位置**：`inclaude/types.h:67-71`
- `std::string` 易拼写错误，应改 `enum class PromptBlockType`

### 5.10【P2】CommandResult::Compact 类型未在 executor 中处理
- **位置**：`inclaude/types.h:48, 54` + `source/executor.cpp:36-58`
- 定义了但 executor 完全未处理，调用方需自行处理

### 5.11【P2】CommandBase 的 setter 非线程安全
- **位置**：`inclaude/command.h:80-82`
- 跨线程修改 `is_enabled_`（std::function）会数据竞争

### 5.12【P2】ExecutorResult 的 next_input / submit_next_input 是死代码
- **位置**：`inclaude/executor.h:25-26` + `source/executor.cpp:54-58`
- 定义但从未设置，"链式输入"功能未实现

### 5.13【P3】ProviderPreset 模型版本可能已过时
- **位置**：`model/provider_preset.cpp:19, 27`
- OpenAI `gpt-4o`、Anthropic `claude-sonnet-4-20250514`（2025 年 5 月版本）
- 当前 2026 年 7 月，建议改从配置文件加载

### 5.14【P3】find_preset 使用线性搜索
- **位置**：`model/provider_preset.cpp:74-78`
- 预设 7 个时影响可忽略，但扩展后无优化空间

---

## 6. tui/core 与 tui/render 模块（终端渲染）

### 6.1【P0】Markdown 代码块使用禁止的四角框线字符（违反硬约束）
- **位置**：`tui/render/markdown_renderer.cpp:626-632`
- 使用 `BOX_TL/BOX_TR/BOX_BL/BOX_BR`（`┌┐└┘`）
- **项目硬约束**明确要求"代码块渲染禁止使用四角框线字符 (┌─┐└┘)，但允许使用 │ 作为左侧连接竖线"
- 应改为左侧 `│` 连接 + 行号格式

### 6.2【P0】Screen::draw_box 垂直循环使用宽度参数
- **位置**：`tui/core/screen.cpp:107`
  ```cpp
  for (int r = row + 1; r < row + 1 + inner_w - 2; r++)
  ```
- 用 `inner_w`（宽度）限制垂直行循环，应用 `inner_h`
- 函数签名本身缺 `height` 参数
- 高瘦盒子绘制错误

### 6.3【P0】Cell 空判定使用 `"\0"` 字符串比较
- **位置**：`tui/core/screen.cpp:229`
  ```cpp
  if (cell.width == 0 && cell.ch == "\0")
  ```
- `"\0"` 是长度 1 的字符串字面量，`cell.ch` 几乎永远不等于它
- 应改 `cell.ch.empty()` 或 `cell.ch[0] == '\0'`
- 导致空白单元格永不被识别，差分渲染输出大量无效空格

### 6.4【P0】LineEditor::estimate_width 始终返回 1
- **位置**：`tui/input/line_editor.cpp:136-141`
  ```cpp
  int LineEditor::estimate_width(char32_t codepoint) {
      (void)codepoint;
      return 1;
  }
  ```
- 完全忽略 codepoint，CJK 字符（宽 2）、emoji（宽 2）光标定位全错
- Backspace、Left/Right、插入位置都会错位
- 严重影响中文用户体验

### 6.5【P0】LineEditor Backspace 删除宽字符只清 1 个空格
- **位置**：`line_editor.cpp:495-523`
- 因 estimate_width=1，删除 CJK 时只输出一个 `\b \b`，遗留空格

### 6.6【P1】DisplayBuffer::handle_csi 支持序列极其有限
- **位置**：`tui/core/display_buffer.cpp:84-91`
- 只处理 `m`（SGR）和 `J`（仅参数 `"2"`）
- 忽略 `K`（擦除行）、`H`/`f`（光标定位）、`r`（滚动区域）、`s`/`u`（保存/恢复光标）
- `\x1b[2J\x1b[H` 清屏时 DisplayBuffer 无法同步镜像状态

### 6.7【P1】`\r` 处理仅重置列不清空行构建器
- **位置**：`display_buffer.cpp:107-109`
- 处理 `\r` 时只 `m_col = 0`，未清 `m_row_builder`
- 进度条 `\r` 重绘时行内容被污染

### 6.8【P1】SGR 栈硬编码阈值 64
- **位置**：`display_buffer.cpp:80`
- 嵌套较深语法高亮（Diff 背景 + 多层着色）触发栈溢出丢弃，后续样式丢失

### 6.9【P1】scroll_h 硬编码底部 3 行假设
- **位置**：`display_buffer.cpp:142`
- 假定底部固定保留 3 行，status_bar 高度变化时滚动区域计算错误

### 6.10【P1】ChatRenderer 清屏未重置 DisplayBuffer
- **位置**：`tui/render/chat_renderer.cpp:704`
- `write("\x1b[2J\x1b[H")` 后未通知 DisplayBuffer 清空，覆盖层恢复基于过时数据

### 6.11【P1】ChatRenderer 打字机效果 15ms 卡顿
- **位置**：`chat_renderer.cpp:716-722`
- `sleep_for(15ms)` 每字符，1000 字符累积 15 秒延迟
- 应改 StreamingBuffer 60fps 节流

### 6.12【P1】ChatRenderer token 计数重复累加
- **位置**：`chat_renderer.cpp:451, 491`
- `StreamTokenEvent` 已累加 `token_count`，`StreamEndEvent` 又累加 `generated_tokens`（总生成数）→ 翻倍

### 6.13【P1】markdown_renderer 变量命名错误：inner_h 实为宽度
- **位置**：`tui/render/markdown_renderer.cpp:574-641`
  ```cpp
  int inner_h = max_w + 2;  // 实际是宽度，应为 inner_w
  ```
- 后续 `h_after = inner_h - 1 - display_width(lang) - 2` 用错值算高度

### 6.14【P1】syntax_highlighter Diff 中语法高亮回退逻辑错误
- **位置**：`tui/render/syntax_highlighter.cpp:514-518`
- `hl_lines.size() != lines.size()` 回退无高亮
- 但 `lines` 包含 header 行（`@@`、`---`），`hl_lines` 仅代码行，size 必然不匹配
- 回退几乎总是触发，等于禁用 Diff 中语法高亮

### 6.15【P1】syntax_highlighter 空行无背景色
- **位置**：`syntax_highlighter.cpp:484-487`
- 空行 `prefix=0` → `bg=nullptr`，Diff 块中空行无背景色，视觉断裂

### 6.16【P1】UTF-8 宽度判定范围不全
- **位置**：`tui/utils/utf8_utils.cpp:33-44`
- 缺少 `0x1F300-0x1FAFF`（emoji）、`0x2600-0x26FF`、`0x2700-0x27BF`、`0x2B00-0x2BFF`
- CJK 扩展 E/F/G/H/I 等区域

### 6.17【P1】UTF-8 Tab 宽度为 0
- **位置**：`utf8_utils.cpp:22`
- 控制字符 `\t` 宽度返回 0，行尾对齐错误

### 6.18【P1】4 字节字符一律 width=2，未处理 ZWJ 序列
- **位置**：`utf8_utils.cpp:47`
- `👨‍👩‍👧‍👦` 应整体宽度 2，但每个组成字符各计 2 → 总宽度 8+

### 6.19【P1】SelectPanel 使用字节 size 作为列位置
- **位置**：`tui/widgets/select_panel.cpp:192`
- `m_screen->write(row, 5 + m_input_buffer.size(), ...)`
- CJK 字符每个占 2 列但只 +1，光标列位置错误

### 6.20【P1】strip_ansi 未处理 OSC 序列
- **位置**：`markdown_renderer.cpp:165-175`
- 只处理 CSI（`\x1b[`），忽略 OSC（`\x1b]...\x07`）
- 设置终端标题的 OSC 序列会泄漏到显示中

### 6.21【P1】OutputFormatter 未处理 `\r` 字符
- **位置**：`tui/render/output_formatter.cpp:171`
- 流式输出中 `\r` 污染行内容

### 6.22【P1】OutputFormatter 未处理 4 空格缩进代码块
- **位置**：`output_formatter.cpp:75`
- 仅检测 ``` 围栏，不识别 Markdown 标准 4 空格缩进代码块

### 6.23【P2】`**bold**` 渲染使用黄色而非粗体
- **位置**：`markdown_renderer.cpp:429-437`
- 输出 `ansi::YELLOW` 而非 `ansi::BOLD`，违反 Markdown 语义

### 6.24【P2】TableBuffer::feed_line Invalid 状态可能丢失表头
- **位置**：`markdown_renderer.cpp:308-343`
- 首行因前导空行被识别为 Invalid 时整张表渲染失败

### 6.25【P2】OutputFormatter bullet 检测未处理 `+` 列表
- **位置**：`output_formatter.cpp:124-132`
- CommonMark 允许 `-`/`*`/`+`，只识别前两种

### 6.26【P2】OutputFormatter 表格渲染未考虑缩进
- **位置**：`output_formatter.cpp:285-289`
- 嵌套在引用块/列表中的表格宽度计算错误

### 6.27【P2】syntax_highlighter Diff 行 prefix 用 char 类型 -1 判定
- **位置**：`syntax_highlighter.cpp:478`
- `prefix` 为 `char`，`-1` 等于 `0xFF`，UTF-8 中字节 `0xFF` 可能误判

### 6.28【P2】classify_by_type 子串匹配可能误判
- **位置**：`syntax_highlighter.cpp:285-304`
- `find("number")` 同时匹配 `"number_literal"` 和 `"number"`

### 6.29【P2】LineEditor::read_line 未重置历史状态
- **位置**：`line_editor.cpp:337`
- 进入循环未重置 `m_history_idx`/`m_backup_line`
- 上次浏览到历史中间项后新输入从中间项开始

### 6.30【P2】LineEditor is_special_char 反色显示未考虑多字节
- **位置**：`line_editor.cpp:562-569`
- UTF-8 序列可能被拆开显示

### 6.31【P2】ChatRenderer 硬编码列位置
- **位置**：`chat_renderer.cpp:502-506`
- `cursor_to_pos(input_row, 3)` 硬编码列 3

### 6.32【P2】SetupWizard 硬编码屏幕尺寸与 ASCII 输入
- **位置**：`tui/setup/setup_wizard.cpp:57, 286-289, 294`
- `m_screen->resize(80, 30)` 假定 80x30
- 退格 `write("\b \b")` 假定字符宽 1
- `ch >= 0x20 && ch < 0x7F` 只接受 ASCII，无法输入中文 API Key

### 6.33【P3】Terminal::end_overlay 使用非标准 `\x1b[s/u`
- **位置**：`tui/core/terminal.cpp:528-553`
- 部分终端不支持，应改 `\x1b7`/`\x1b8`（DECSC/DECRC）

### 6.34【P3】is_list_item 中 `dot > 0` 多余
- **位置**：`markdown_renderer.cpp:536-540`
- `size_t` 无符号，`npos` 也 > 0，永远为 true

### 6.35【P3】std::string buf(code) 不必要拷贝
- **位置**：`syntax_highlighter.cpp:589-592`
- 大文件高亮时频繁堆分配

---

## 7. tui/input 与平台抽象模块

### 7.1【P0】Win32 平台编码不一致
- **位置**：`tui/core/platform/platform_win32.cpp:58`
  ```cpp
  _setmode(_fileno(stdin), _O_WTEXT);  // stdin 设为 UTF-16
  ```
- 但 `write_output` 用 `WriteConsoleA` 写 UTF-8 字节
- stdin 与 stdout 编码不匹配，输入输出混用时字符显示乱码

### 7.2【P1】POSIX 平台 ESC 单字符读取会阻塞
- **位置**：`platform/platform_posix.cpp:71-72`
- `read(STDIN_FILENO, seq, 1)` 阻塞读取
- 用户只按 ESC 后程序阻塞在 read 等待后续字节，UI 无响应
- 应用 `select`/`poll` + VTIME 超时

### 7.3【P1】POSIX 平台禁用 ISIG，Ctrl+C 不产生 SIGINT
- **位置**：`platform_posix.cpp:43`
- `raw.c_lflag &= ~ISIG` 后 Ctrl+C 不发 SIGINT
- 若 LineEditor 未处理 0x03，用户无法中断当前操作

### 7.4【P1】POSIX 平台 \n → \r\n 转换不完整
- **位置**：`platform_posix.cpp:152-174`
- `\r` 和 `\n` 间隔 ANSI 序列时（如 `\r\x1b[K\n`）转换失败

### 7.5【P1】Win32 平台 ReadConsoleInputW 未处理 key release
- **位置**：`platform_win32.cpp:88-90`
- 未区分 key press/release，可能同一按键处理两次
- 失败返回 `WEOF` 未处理

### 7.6【P1】Win32 SetConsoleOutputCP 未检查返回值
- **位置**：`platform_win32.cpp:54`
- 旧版 Windows 或受限环境失败时 UTF-8 输出乱码但程序不知情

### 7.7【P1】Terminal 事件泵线程启动时机存在竞态
- **位置**：`tui/core/terminal.cpp:87-93`
- 线程在 `m_initialized = true` 之前启动
- 线程内可能访问未初始化完成的成员

### 7.8【P2】Win32 SHORT vs int 比较可能溢出
- **位置**：`platform_win32.cpp:194`
- `initial_pos.X`（SHORT 16 位）与 `viewport_width - 1`（int）比较
- `viewport_width - 1 > 32767` 时结果错误（罕见）

### 7.9【P2】Win32 flush 在 VT 模式下是 no-op
- **位置**：`platform_win32.cpp:174-178`
- `fflush(stdout)` 在 VT 模式下是 no-op
- 混用 `printf` 和 `WriteConsole` 时缓冲区未刷新导致输出顺序错乱

### 7.10【P3】Terminal::initialize 使用 USERPROFILE 在非 Windows 不存在
- **位置**：`terminal.cpp:62-66`
- POSIX 系统应使用 `HOME`（虽有 `#ifdef _WIN32` 保护）

---

## 8. core 与 app 模块（事件总线、任务管理、配置、main）

### 8.1【P0】EventBus 使用 recursive_mutex 可能死锁
- **位置**：`core/events/event_bus.h:69-82`
- subscribe 与 publish 都用 recursive_mutex
- 回调中再次 publish 会持锁执行用户代码，可能死锁或破坏不变量

### 8.2【P0】非 std::exception 异常导致 terminate
- **位置**：`event_bus.h:110-115`
- `catch (const std::exception&)` 吞标准异常
- 抛 `int`/`const char*` 等非 std::exception 时触发 `std::terminate`

### 8.3【P0】publish_async lambda 捕获 this，单例销毁后调用 UB
- **位置**：`event_bus.h:118-124`
- 程序退出时单例销毁后异步线程仍可能调用 lambda
- 访问已释放的 `this`

### 8.4【P0】TaskManager::start 中 detach 线程
- **位置**：`core/task/task_manager.cpp:134-137`
  ```cpp
  std::thread([task]() { task->execute(); }).detach();
  ```
- detach 后线程脱离管理
- TaskManager 销毁时 task 仍运行，访问 EventBus 单例（可能已销毁）等 UB

### 8.5【P0】Task::cancel 状态机不完整
- **位置**：`task_manager.h:68-71`
  ```cpp
  void cancel() {
      m_should_cancel = true;
      m_status = TaskStatus::Cancelled;  // 直接设 Cancelled
  }
  ```
- task 实际可能还在 Running
- `isFinished()` 立即返回 true，`waitForAll` 提前退出，但 task 线程仍在跑

### 8.6【P0】TaskManager::~TaskManager 调用 cancelAll 不等待结束
- **位置**：`task_manager.cpp:99-102`
- `cancelAll` 只设 cancel 标志，`waitForAll` 通过 `isFinished` 判断
- detach 线程可能尚未执行到检查 cancel 处，waitForAll 提前返回

### 8.7【P0】main.cpp TaskManager::cancelAll 调用顺序错误
- **位置**：`app/main.cpp:455-456`
- `cancelAll` 在 `terminal.restore()` 之后调用
- cancelAll 触发的 TaskCompleted/TaskCancelled 事件尝试更新 UI
- 但终端已恢复原始模式，UI 更新写入转义序列到原始终端，屏幕乱码

### 8.8【P0】main.cpp initialize 失败未调用 restore
- **位置**：`app/main.cpp:127-132`
- `terminal.initialize()` 失败时直接返回错误码
- 若已设 raw 模式但后续步骤失败，终端残留 raw 模式，用户 shell 无法正常使用

### 8.9【P1】main.cpp UserInputEvent 同步调用阻塞事件循环
- **位置**：`app/main.cpp:384-440`
- `session->send_message` 同步调用阻塞事件循环
- LLM 响应期间无法处理其他事件（取消、UI 更新）

### 8.10【P1】main.cpp backend 创建失败未清理 EventBus
- **位置**：`app/main.cpp:200-211`
- 已订阅的回调可能捕获局部变量（如 terminal 引用）
- 异步事件触发时访问已失效引用

### 8.11【P1】EventBus clear() 未清除 m_async_queue
- **位置**：`event_bus.h:138-141`
- clear() 清订阅者但不清队列
- 退出时残留异步事件仍分发，订阅者已清除 → 访问已失效捕获变量

### 8.12【P1】ChatRenderer m_pending_tool_calls 查找依赖事件顺序
- **位置**：`chat_renderer.cpp:595-601`
- 通过 tool_call_id 查找，`ToolCallEndEvent` 先于 `StartEvent` 到达时查找失败

### 8.13【P1】flatten_json 中 double 分支不可达
- **位置**：`core/config/config_manager.cpp:125`
- `value.is_number()` 匹配所有数值，`is_number_float()` 分支永远不达
- 配置文件 `0.5` 被当整数存储

### 8.14【P1】load_from_file 未先 clear，多次加载累积
- **位置**：`config_manager.cpp:97-142`
- `flatten_json` 直接修改 `m_values`，多次调用累积键值，无法覆盖同名键

### 8.15【P1】cli_args 未知 provider 直接 exit(1) 无资源清理
- **位置**：`app/config/cli_args.cpp:42, 97`
- `exit(1)` 不调用 RAII 析构（如 Terminal::restore）
- 终端残留 raw 模式

### 8.16【P1】app_config std::stoi 异常处理不全
- **位置**：`app/config/app_config.cpp:136-140`
- 可能抛 `invalid_argument`/`out_of_range`，只捕获 `WORKX_TIMEOUT`
- 其他异常导致崩溃

### 8.17【P1】StatusBar 硬编码 4096 最大 token
- **位置**：`tui/widgets/status_bar.cpp:195`
  ```cpp
  int ctx_pct = std::min(m_token_count * 100 / 4096, 100);
  ```
- 不同模型（128K、200K）的百分比显示全错

### 8.18【P2】TaskManager::waitForAll 使用 busy-wait
- **位置**：`task_manager.cpp:168-181`
- `while + sleep_for(10ms)` 忙等
- 应改 `std::condition_variable`

### 8.19【P2】StatusBar 每秒重绘
- **位置**：`status_bar.cpp:96`
- 时间戳每秒变化导致 `bar` 字符串每秒不同，触发全屏重绘

### 8.20【P2】ConfigScope::get 调用不存在的重载
- **位置**：`config_manager.h:172-180`
- `ConfigManager::get<T>(key, default_value)` 重载不存在
- 可能编译错误或隐式转换问题

### 8.21【P2】app_config 默认路径回退当前目录
- **位置**：`app_config.cpp:159-173`
- 无 `APPDATA` 环境变量时回退当前目录
- 从快捷方式或不同工作目录启动时配置文件丢失

### 8.22【P2】CommandPanel/FileSearchPanel 硬编码布局
- **位置**：`tui/widgets/command_panel.cpp:107, 145`、`file_search_panel.cpp:107-111`
- `clear_start = height - 1 - MAX_DISPLAY` 可能为负
- `padding = 16 - ...` 硬编码 16

### 8.23【P2】SelectPanel 未处理面板超出屏幕
- **位置**：`select_panel.cpp:147`
- `start_row = term_h - panel_h - 3` 未处理 `panel_h > term_h`
- 小屏幕上 start_row 为负数，写入失败或越界

### 8.24【P3】m_total_tokens 未保护负数
- **位置**：`chat_renderer.cpp:451`
- 未检查 `e.token_count` 是否为负

---

## 9. 优先级修复清单（汇总建议）

### P0 必修（17 项）

#### 并发与生命周期（核心）
1. ChatSession 多线程无同步（2.1）
2. ChatSession 后台 lambda 捕获 this 析构不等待（2.2）
3. ChatSession agent 循环无 try-catch（2.3）
4. Client `m_messages` 无同步（4.3）
5. Client `stream_chat_async` TOCTOU（4.4）
6. Client 捕获 this 析构悬空（4.5）
7. TaskManager detach 线程（8.4）
8. Task::cancel 状态机不完整（8.5）
9. TaskManager 析构不等待（8.6）
10. EventBus recursive_mutex 死锁（8.1）
11. EventBus 非 std::exception 触发 terminate（8.2）
12. EventBus publish_async 单例销毁后 UB（8.3）

#### 数据完整性
13. `chat_async` 双重包装导致重复入历史（4.1）
14. 重试未清空输出导致拼接错乱（4.2）
15. FileWriteTool 写入失败数据丢失（3.2）
16. LCS diff O(n*m) 内存 OOM（3.3）

#### 安全
17. SSE parser 日志泄露全部响应内容（4.7）
18. list_models 日志泄露 Anthropic x-api-key（4.8）

#### 渲染致命 bug（独立子领域）
19. Markdown 代码块用禁止的四角框线字符（6.1）— 违反项目硬约束
20. Screen::draw_box 垂直循环用宽度参数（6.2）
21. Cell 空判定用 `"\0"` 字符串比较（6.3）
22. LineEditor::estimate_width 始终返回 1（6.4）— 中文用户体验致命
23. Win32 平台编码不一致（7.1）
24. main.cpp TaskManager::cancelAll 顺序错误（8.7）
25. main.cpp initialize 失败未 restore（8.8）
26. ChatSession regenerate 未检查 generating（2.4）

#### 工具执行
27. ToolExecutor::execute 无 try-catch（3.1）
28. `input.get<T>()` 无 try-catch（3.4）
29. 配置值绕过文件大小限制（3.5）
30. `fs::exists` 失败绕过 pre-read（3.6）

### P1 应尽快修（约 35 项，散见各章节）

主要包括：
- agent/core：重试 m_generating 错误重置、max_iterations 丢失输出、processor 各 stub bug
- agent/tool：各种 limit 溢出、GlobTool 限制缺失、registry 重复注册、内存序
- agent/api：API 适配器多个错误（list_models、tool_calls、content_filter、reasoning、tool_result 合并等）
- agent/command：dynamic_cast 反模式、parse 引号/空格 bug、registry nullptr
- tui：DisplayBuffer CSI 支持不全、`\r` 处理、UTF-8 宽度范围、打字机卡顿、token 重复累加、Diff 高亮回退错误
- core/app：UserInputEvent 阻塞、配置加载累积、exit 无清理、stoi 异常

### P2/P3 视情况修（约 50+ 项）

主要包括：
- 函数过长、代码重复、命名/注释不一致
- 性能优化（std::regex、substr 重建、不必要拷贝）
- 现代特性（std::variant、std::optional、string_view）
- 14 个 stub 模块的落地规划

---

## 10. 总体评估与建议方向

### 10.1 项目当前状态
- **架构骨架完整**：Agent → Tool → LLM Backend → TUI 分层清晰
- **核心路径可运行**：基础聊天 + 文件读写 + diff 渲染流程可用
- **关键能力缺失**：权限、压缩、提示词、模型路由等 Agent 必备能力全是 stub
- **存在多个 P0 bug**：并发安全、数据完整性、安全日志、渲染核心均有致命问题

### 10.2 推荐修复顺序（不执行，仅建议）

**阶段 1：稳定性与安全（P0 优先）**
- 修复 ChatSession/Client/TaskManager/EventBus 的并发与生命周期问题
- 修复 FileWriteTool 原子写入与 LCS OOM
- 修复日志泄露
- 引入 ToolExecutor try-catch 与权限校验骨架

**阶段 2：渲染核心（违反硬约束 + 中文支持）**
- Markdown 代码块改为 `│` 竖线格式（符合 project_memory 硬约束）
- 修复 Screen::draw_box、Cell 空判定、LineEditor estimate_width
- 修复 Win32 编码不一致

**阶段 3：API 适配正确性**
- 修复 OpenAI/Anthropic 适配器的 17 个具体 bug
- 启用 `stream_options.include_usage`
- Anthropic list_models 分发
- 多 tool_result 合并

**阶段 4：stub 落地**
- 优先 permission + json_schema（安全前置）
- 然后 compact + model/config（长会话可用性）
- 然后 prompt（系统提示词构建）

**阶段 5：代码质量**
- 拆分长函数、抽取重复代码
- 现代特性改造
- 性能优化

### 10.3 风险提示
- 部分修复（如加 mutex）可能引入死锁，需配合充分的并发测试
- API 适配器修复需要真实 API Key 验证，建议加集成测试
- stub 落地是大工程，需单独评估优先级与依赖关系
- 项目内存有"代码块格式硬约束"，渲染层修复需严格遵守

---

## 11. 待审批事项

请审阅本报告并确认：

1. **是否同意本报告的问题分级与修复优先级？**
2. **是否需要补充其他维度的分析？**（如测试覆盖、依赖管理、构建系统）
3. **是否进入 Plan 阶段？** 若是，请指示：
   - (a) 一次性针对全部 P0 + 部分 P1 写修复 Plan
   - (b) 分阶段写 Plan（先稳定性 P0，再渲染 P0，再 API P1...）
   - (c) 只针对你最关心的某几个模块写 Plan

待你审批后，我会写一份详细的执行 Plan（仍不修改代码），再次由你审批后才进入执行阶段。
