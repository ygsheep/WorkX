# llama.cpp TUI 实现分析

> 对 llama.cpp 三个终端交互式工具的架构与实现进行全面剖析

---

## 1. 项目概览

llama.cpp 包含 **三个不同层级** 的终端交互工具，从简到繁：

| 工具 | 路径 | 定位 | 行编辑 | 推理架构 |
|------|------|------|--------|---------|
| `llama-run` | `tools/run/` | 最简聊天入口 | linenoise (POSIX) / getline (Win) | 直接同步循环 |
| `llama-cli` | `tools/cli/` | 功能完整的聊天客户端 | 自研 console::readline | Server 任务队列 |
| `llama-completion` | `tools/completion/` | 底层 token 级交互 | 自研 console::readline | 直接同步循环 |

本文以 `llama-cli` 为主进行深入分析，并在第 10 节给出三者对比。

**源码位置：**

| 文件 | 说明 |
|------|------|
| `tools/cli/main.cpp` | CLI 程序入口，仅调用 `llama_cli()` |
| `tools/cli/cli.cpp` | 全部 TUI 逻辑，约 655 行 |
| `tools/run/run.cpp` | llama-run 全部逻辑，约 1409 行 |
| `tools/run/linenoise.cpp/linenoise.cpp` | linenoise 行编辑库 (POSIX)，约 1995 行 |
| `tools/completion/completion.cpp` | llama-completion 全部逻辑，约 1007 行 |
| `common/console.h` | 控制台抽象层头文件 |
| `common/console.cpp` | 控制台底层实现，约 1167 行（核心 TUI 引擎） |
| `tools/server/server-context.h/.cpp` | 推理引擎封装（CLI 复用） |
| `tools/server/server-task.h/.cpp` | 任务/响应数据结构 |
| `common/chat.h` | 聊天模板与消息解析 |

**构建依赖链：**

```
llama-cli (可执行文件)
  └── llama-cli-impl (静态库)
        ├── server-context (推理引擎)
        ├── llama-common (公共库, 含 console.cpp)
        └── thread lib

llama-run (可执行文件)
  └── 内含 linenoise.cpp (POSIX 行编辑)
        ├── llama (核心库)
        ├── llama-common (公共库, 不含 console.cpp)
        └── curl / http (模型下载)

llama-completion (可执行文件)
  └── llama-completion-impl (静态库)
        ├── llama (核心库)
        ├── llama-common (公共库, 含 console.cpp)
        └── thread lib
```

---

## 2. 整体架构

```
┌──────────────────────────────────────────────────────────┐
│                      用户终端                              │
└──────────────────────┬───────────────────────────────────┘
                       │ ANSI escape / Win32 Console API
┌──────────────────────▼───────────────────────────────────┐
│               console 命名空间 (common/console.cpp)        │
│  ┌─────────────┐  ┌──────────────┐  ┌────────────────┐  │
│  │ readline()  │  │ set_display()│  │ spinner        │  │
│  │ (行编辑器)  │  │ (颜色控制)   │  │ (加载动画)     │  │
│  └──────┬──────┘  └──────┬───────┘  └────────────────┘  │
│         │                │                                │
│  ┌──────▼────────────────▼──────┐  ┌──────────────────┐  │
│  │    ANSI Color / Win32 API    │  │ completion_cb    │  │
│  │    (平台抽象)                │  │ (Tab 补全回调)   │  │
│  └──────────────────────────────┘  └──────────────────┘  │
└──────────────────────┬───────────────────────────────────┘
                       │
┌──────────────────────▼───────────────────────────────────┐
│              cli_context (tools/cli/cli.cpp)               │
│  ┌───────────────────┐  ┌─────────────────────────────┐  │
│  │ messages (JSON)   │  │ generate_completion()       │  │
│  │ (对话历史)        │  │ (流式推理 + 输出渲染)       │  │
│  └───────────────────┘  └──────────────┬──────────────┘  │
│  ┌───────────────────┐                 │                  │
│  │ input_files       │                 │                  │
│  │ (多模态附件)      │                 │                  │
│  └───────────────────┘                 │                  │
│  ┌───────────────────┐                 │                  │
│  │ format_chat()     │                 │                  │
│  │ (模板渲染)        │                 │                  │
│  └───────────────────┘                 │                  │
└─────────────────────────┬──────────────┘
                          │
┌─────────────────────────▼──────────────────────────────────┐
│                server_context (推理引擎)                     │
│  ┌──────────────────┐  ┌────────────────────────────────┐ │
│  │ load_model()     │  │ start_loop() [推理线程]        │ │
│  │ get_response_    │  │ server_response_reader         │ │
│  │ reader()         │  │ (流式读取 partial/final 结果) │ │
│  └──────────────────┘  └────────────────────────────────┘ │
└────────────────────────────────────────────────────────────┘
```

---

## 3. 程序启动流程

```cpp
// main.cpp — 极简入口
int main(int argc, char ** argv) {
    return llama_cli(argc, argv);
}
```

`llama_cli()` 的启动序列：

```
1. 参数解析        common_params_parse() → common_params
2. 创建 CLI 上下文  cli_context ctx_cli(params)
3. 初始化后端       llama_backend_init() / llama_numa_init()
4. 初始化控制台     console::init(simple_io, use_color)
5. 注册信号处理     SIGINT/SIGTERM → signal_handler() → g_is_interrupted
6. 加载模型        ctx_server.load_model(params) + spinner 动画
7. 启动推理线程    std::thread inference_thread → ctx_server.start_loop()
8. 显示欢迎信息    ASCII Logo + 模型信息 + 可用命令
9. 进入交互循环    while(true) { readline → process → generate }
```

---

## 4. 核心模块详解

### 4.1 控制台抽象层 (`console` 命名空间)

这是 TUI 的核心基础设施，位于 `common/console.cpp`，约 1167 行，承担所有终端交互。

#### 4.1.1 双模式 IO

| 模式 | 触发条件 | 实现 | 能力 |
|------|---------|------|------|
| **Simple IO** | `--simple-io` 参数 / 控制台不可用 | `readline_simple()` | 仅 `std::getline`，无行编辑 |
| **Advanced IO** | 默认（检测到终端） | `readline_advanced()` | 完整行编辑器 |

#### 4.1.2 行编辑器 (`readline_advanced`)

这是一个**自研的行编辑器**，不依赖 readline/libedit，完全手动处理字符级输入。

**支持的编辑操作：**

| 操作 | POSIX 键绑定 | Windows 键绑定 |
|------|-------------|---------------|
| 光标左移 | `←` / `\033[D` | `KEY_ARROW_LEFT` |
| 光标右移 | `→` / `\033[C` | `KEY_ARROW_RIGHT` |
| 左移一词 | `Ctrl+←` / `\033[1;5D` | `KEY_CTRL_ARROW_LEFT` |
| 右移一词 | `Ctrl+→` / `\033[1;5C` | `KEY_CTRL_ARROW_RIGHT` |
| 行首 | `Home` / `\033[H` | `KEY_HOME` |
| 行尾 | `End` / `\033[F` | `KEY_END` |
| 退格 | `0x08` / `0x7F` | 同 POSIX |
| 删除 | `\033[3~` | `KEY_DELETE` |
| 历史上翻 | `↑` / `\033[A` | `KEY_ARROW_UP` |
| 历史下翻 | `↓` / `\033[B` | `KEY_ARROW_DOWN` |
| Tab 补全 | `\t` | `\t` |
| 多行续行 | `\` 或 `/` 结尾 | 同 POSIX |

**关键实现细节：**

- **双位置追踪**：同时维护 `byte_pos`（字节偏移）和 `char_pos`（字符偏移），正确处理 UTF-8 多字节字符
- **宽度追踪**：`std::vector<int> widths` 记录每个字符的显示宽度，处理 CJK 宽字符
- **中间插入**：在行中间插入字符时，重绘尾部内容并回退光标
- **历史管理**：`history_t` 结构，支持上/下翻浏览，自动去重

#### 4.1.3 Unicode 处理

```
getchar32()           → 读取一个 Unicode 码点（处理 UTF-16 surrogate pair）
decode_utf8()         → 从 UTF-8 字节流解码码点
append_utf8()         → 将码点编码为 UTF-8
estimateWidth()       → 估算字符显示宽度（POSIX 用 wcwidth，Windows 固定 1）
put_codepoint()       → 输出码点到终端并返回实际显示宽度
```

Windows 平台通过 `ReadConsoleInputW` 读取 Unicode 输入，POSIX 通过 `getwchar()`。

#### 4.1.4 颜色系统

```cpp
enum display_type {
    DISPLAY_TYPE_RESET,      // 默认色
    DISPLAY_TYPE_INFO,       // 品红 (Magenta)
    DISPLAY_TYPE_PROMPT,     // 黄色 (Yellow)
    DISPLAY_TYPE_REASONING,  // 灰色 (Gray) — 思考/推理内容
    DISPLAY_TYPE_USER_INPUT, // 绿色加粗 (Bold Green)
    DISPLAY_TYPE_ERROR,      // 红色加粗 (Bold Red)
};
```

使用 ANSI 转义码实现，Windows 通过 `ENABLE_VIRTUAL_TERMINAL_PROCESSING` 启用 VT100 支持。`set_display()` 会追踪当前颜色状态，仅在变化时发送转义码，避免冗余输出。

#### 4.1.5 加载动画 (Spinner)

```cpp
namespace spinner {
    static const char LOADING_CHARS[] = {'|', '/', '-', '\\'};
    void start();  // 启动独立线程，每 100ms 切换帧
    void stop();   // 停止线程，清除动画字符
}
```

使用 `std::condition_variable` 控制线程生命周期，通过 `replace_last()` 实现原地刷新动画。

#### 4.1.6 平台抽象

| 能力 | POSIX | Windows |
|------|-------|---------|
| 原始模式 | `termios` 关闭 ICANON/ECHO | `SetConsoleMode` 关闭 ENABLE_LINE_INPUT/ECHO_INPUT |
| 光标移动 | `\b` / `\033[C` ANSI 转义 | `SetConsoleCursorPosition` Win32 API |
| 输出设备 | `/dev/tty` (独立于 stdout) | `STD_OUTPUT_HANDLE` |
| 字符宽度 | `wcwidth()` | 固定返回 1 |
| 字符输出 | `fwrite` | `WriteConsole` |
| 信号处理 | `sigaction(SIGINT)` | `SetConsoleCtrlHandler` |

---

### 4.2 CLI 交互循环 (`cli_context`)

#### 4.2.1 对话状态管理

```cpp
struct cli_context {
    server_context ctx_server;          // 推理引擎
    json messages = json::array();      // 对话历史 (OpenAI 兼容格式)
    std::vector<raw_buffer> input_files; // 多模态附件缓冲
    task_params defaults;               // 默认采样参数
    bool verbose_prompt;
    std::atomic<bool> loading_show;     // 加载动画控制
};
```

**消息格式**：使用 JSON 数组存储，每条消息为 `{"role": "system|user|assistant", "content": "..."}`

#### 4.2.2 交互主循环

```cpp
while (true) {
    // 1. 读取用户输入
    console::set_display(DISPLAY_TYPE_USER_INPUT);
    console::log("\n> ");
    do {
        another_line = console::readline(line, params.multiline_input);
        buffer += line;
    } while (another_line);

    // 2. 命令解析与分发
    if (buffer == "/exit") break;
    else if (buffer == "/regen") { /* 删除最后一轮 */ }
    else if (buffer == "/clear") { /* 清空历史 */ }
    else if (buffer == "/read <f>") { /* 加载文本文件 */ }
    else if (buffer == "/glob <p>") { /* 批量加载文件 */ }
    else if (buffer == "/image <f>") { /* 加载图片 */ }
    else if (buffer == "/audio <f>") { /* 加载音频 */ }
    else { cur_msg += buffer; }

    // 3. 构造用户消息并推理
    ctx_cli.messages.push_back({"role", "user", "content", cur_msg});
    std::string response = ctx_cli.generate_completion(timings);
    ctx_cli.messages.push_back({"role", "assistant", "content", response});

    // 4. 显示推理耗时
    if (params.show_timings) { /* Prompt/G速度 */ }
}
```

#### 4.2.3 命令系统

| 命令 | 功能 | 实现 |
|------|------|------|
| `/exit` | 退出程序 | `break` 主循环 |
| `/regen` | 重新生成最后回复 | 删除最后一条 assistant 消息，保留 user 消息 |
| `/clear` | 清空对话历史 | `messages.clear()` + 重新添加 system prompt |
| `/read <file>` | 加载文本文件到消息 | 读取文件内容拼接到 `cur_msg` |
| `/glob <pattern>` | 批量加载文件 | `recursive_directory_iterator` + `glob_match`，上限 100 个 |
| `/image <file>` | 加载图片（需视觉模型） | `load_input_file(fname, true)` → `raw_buffer` |
| `/audio <file>` | 加载音频（需音频模型） | 同上 |

#### 4.2.4 Tab 自动补全

```cpp
static std::vector<std::pair<std::string, size_t>>
auto_completion_callback(std::string_view line, size_t cursor_byte_pos);
```

补全逻辑：
1. **命令补全**：输入 `/` 开头时匹配 `cmds` 数组中的命令前缀
2. **路径补全**：在 `/read`、`/image`、`/audio`、`/glob` 命令后，对文件路径进行补全
3. **最长公共前缀**：多个候选时自动提取 LCP
4. **跨平台路径**：支持 `~` 展开（POSIX）、绝对路径、相对路径

---

### 4.3 流式推理与输出

#### 4.3.1 推理请求构造

```cpp
std::string generate_completion(result_timings & out_timings) {
    server_response_reader rd = ctx_server.get_response_reader();
    auto chat_params = format_chat();  // 应用 chat template

    server_task task(SERVER_TASK_TYPE_COMPLETION);
    task.cli = true;                    // 标记为 CLI 任务
    task.cli_prompt = chat_params.prompt;
    task.cli_files  = input_files;
    task.params.stream = true;          // 强制流式模式

    rd.post_task({std::move(task)});
    // ...
}
```

#### 4.3.2 流式输出处理

```cpp
while (result) {
    // 处理 partial 结果（流式 token）
    auto res_partial = dynamic_cast<server_task_result_cmpl_partial *>(result.get());
    if (res_partial) {
        for (const auto & diff : res_partial->oaicompat_msg_diffs) {
            if (!diff.content_delta.empty()) {
                if (is_thinking) {
                    console::log("\n[End thinking]\n\n");
                    is_thinking = false;
                }
                console::log("%s", diff.content_delta.c_str());
            }
            if (!diff.reasoning_content_delta.empty()) {
                console::set_display(DISPLAY_TYPE_REASONING);
                if (!is_thinking) console::log("[Start thinking]\n");
                is_thinking = true;
                console::log("%s", diff.reasoning_content_delta.c_str());
            }
        }
    }

    // 处理 final 结果（推理完成）
    auto res_final = dynamic_cast<server_task_result_cmpl_final *>(result.get());
    if (res_final) break;

    result = rd.next(should_stop);  // 等待下一个结果
}
```

**输出颜色语义：**
- **正常内容**：默认终端颜色
- **推理/思考内容**：灰色（`DISPLAY_TYPE_REASONING`），带 `[Start thinking]` / `[End thinking]` 标记
- **用户输入**：绿色加粗
- **错误信息**：红色加粗

#### 4.3.3 中断处理

```cpp
// 双击 Ctrl+C 立即退出
static void signal_handler(int) {
    if (g_is_interrupted.load()) {
        fprintf(stdout, "\033[0m\n");  // 清除颜色
        std::exit(130);
    }
    g_is_interrupted.store(true);  // 第一次：设置中断标志
}
```

- 第一次 Ctrl+C：设置 `g_is_interrupted`，推理循环在下次 `rd.next(should_stop)` 时检测到并退出
- 第二次 Ctrl+C：直接 `std::exit(130)` 强制退出

---

### 4.4 聊天模板系统

```cpp
common_chat_params format_chat() {
    auto meta = ctx_server.get_meta();
    auto & chat_params = meta.chat_params;
    auto caps = common_chat_templates_get_caps(chat_params.tmpls.get());

    common_chat_templates_inputs inputs;
    inputs.messages = common_chat_msgs_parse_oaicompat(messages);
    inputs.add_generation_prompt = true;
    inputs.reasoning_format = COMMON_REASONING_FORMAT_DEEPSEEK;
    // ...

    return common_chat_templates_apply(chat_params.tmpls.get(), inputs);
}
```

关键点：
- 使用 Jinja2 模板引擎渲染对话历史为 prompt
- 支持 `reasoning_format`（deepseek 格式，分离思考内容）
- `enable_thinking` 根据模板能力自动判断
- 输出包含 `thinking_start_tag` / `thinking_end_tag` 用于 reasoning budget 控制

---

## 5. 线程模型

```
┌─────────────────────┐     ┌──────────────────────────────┐
│    主线程 (CLI)       │     │    推理线程                    │
│                      │     │                              │
│  readline()          │     │  ctx_server.start_loop()     │
│    ↓                 │     │    ↓                         │
│  命令处理             │     │  等待 task 队列              │
│    ↓                 │     │    ↓                         │
│  generate_completion │     │  处理 completion task        │
│    ↓                 │     │    ↓                         │
│  rd.next(should_stop)│◄────│  推送 partial/final result  │
│    ↓                 │     │    ↓                         │
│  console::log() 输出  │     │  继续下一个 token            │
│    ↓                 │     │                              │
│  回到 readline()     │     │                              │
└─────────────────────┘     └──────────────────────────────┘

┌─────────────────────┐
│  Spinner 线程        │  (加载模型时启动，100ms 刷新)
│  spinner::start()   │
│  spinner::stop()    │
└─────────────────────┘
```

**线程间通信**：
- 主线程 → 推理线程：通过 `server_response_reader::post_task()` 提交任务
- 推理线程 → 主线程：通过 `server_response_reader::next()` 获取结果（阻塞等待）
- 中断信号：通过 `g_is_interrupted` 原子变量传递

---

## 6. 关键设计模式

### 6.1 CLI 复用 Server 推理引擎

`cli_context` 内嵌 `server_context`，**不是直接调用 llama API**，而是复用了 server 的任务队列和推理循环。这带来：

- ✅ 统一的推理逻辑（避免代码重复）
- ✅ 自动的 KV cache 管理
- ✅ 支持 speculative decoding
- ✅ 支持 prompt cache / checkpoint
- ⚠️ 引入了线程间通信开销
- ⚠️ server 代码耦合（`#include "server-common.h"`）

### 6.2 两种 IO 模式策略

`simple_io` 作为降级方案，确保在非终端环境（管道、重定向、IDE 集成终端）中也能正常工作。`advanced_display` 则在支持 ANSI 转义码的终端中启用颜色和行编辑。

### 6.3 流式 Diff 输出

使用 `common_chat_msg_diff` 进行增量输出，而非全量替换。`content_delta` 和 `reasoning_content_delta` 分别追踪正文和推理内容的增量，支持：
- 无缝切换 thinking/content 显示
- 避免重复输出
- 为未来 tool call 流式展示预留接口

---

## 7. 构建自定义 CLI 工具的参考

如果需要基于此项目实现自定义 CLI 工具，以下是关键接口：

### 7.1 必须使用的组件

```cpp
// 1. 控制台初始化
console::init(simple_io, use_color);
console::set_completion_callback(my_completion_cb);

// 2. 行读取
std::string line;
bool has_more = console::readline(line, multiline_input);

// 3. 彩色输出
console::set_display(DISPLAY_TYPE_INFO);
console::log("some message\n");
console::set_display(DISPLAY_TYPE_RESET);

// 4. 推理引擎
server_context ctx_server;
ctx_server.load_model(params);
std::thread([&]() { ctx_server.start_loop(); }).detach();

server_response_reader rd = ctx_server.get_response_reader();
// 构造 server_task 并 post_task()
// 通过 rd.next() 获取 partial/final 结果
```

### 7.2 可选扩展点

| 扩展点 | 方式 | 说明 |
|--------|------|------|
| 新增斜杠命令 | 修改 `cmds` 数组 + 交互循环 `if/else` | 如 `/save`, `/load` |
| 自定义补全 | 修改 `auto_completion_callback` | 如命令参数补全 |
| 自定义输出格式 | 修改 `generate_completion()` 内的渲染逻辑 | 如 Markdown 渲染 |
| 多模态支持 | 通过 `load_input_file(fname, true)` | 已有接口 |
| Reasoning 显示 | 通过 `diff.reasoning_content_delta` | 已有接口 |
| 流式进度条 | 利用 `spinner` 框架 + `result_prompt_progress` | 待实现 |

### 7.3 注意事项

1. **console::log vs LOG_INF**：`console::log()` 直接输出到 stdout，**不应在推理线程调用**（性能问题），推理线程应使用 `log.h` 的 `LOG_INF`
2. **颜色状态**：`set_display()` 会全局修改终端颜色，注意在异常路径上恢复（`DISPLAY_TYPE_RESET`）
3. **UTF-8 假设**：整个系统假设输入/输出为 UTF-8，Windows 通过 `SetConsoleOutputCP(CP_UTF8)` + `_setmode(_O_WTEXT)` 实现
4. **Ctrl+C 安全**：双击退出机制依赖 `g_is_interrupted` 原子变量，自定义代码需检查此标志
5. **server_context 生命周期**：`start_loop()` 会阻塞线程，`terminate()` 会解除阻塞，确保在主线程退出前 join 推理线程

---

## 8. 文件依赖关系图

```
tools/cli/main.cpp
    └── tools/cli/cli.cpp
          ├── common/console.h        (TUI 引擎)
          ├── common/chat.h           (聊天模板)
          ├── common/common.h         (公共工具)
          ├── common/arg.h            (参数解析)
          ├── common/fit.h            (显存适配)
          ├── tools/server/server-context.h  (推理引擎)
          └── tools/server/server-task.h     (任务结构)

common/console.cpp
    ├── common/log.h                  (日志系统)
    └── 平台头文件 (Windows.h / termios.h)
```

---

## 9. 总结

llama-cli 的 TUI 实现是一个**轻量级但功能完备**的终端聊天界面：

- **行编辑器**完全自研（~400 行），支持 UTF-8、CJK 宽字符、历史浏览、Tab 补全
- **双模式 IO** 兼顾高级终端和降级场景
- **流式推理**通过 server_context 复用实现，推理与 UI 在不同线程
- **Reasoning 显示**以灰色区分思考内容，带 `[Start/End thinking]` 标记
- **多模态输入**（图片/音频/文件）通过命令系统支持
- **跨平台**通过 `#if defined(_WIN32)` 分支实现，POSIX 和 Windows 代码路径清晰分离

整个实现约 1800 行（cli.cpp 655 + console.cpp 1167），复杂度适中，适合作为自定义 CLI 工具的参考蓝本。

---

## 10. 三种 TUI 工具对比

| 特性 | llama-run | llama-cli | llama-completion |
|------|-----------|-----------|------------------|
| **定位** | 最简聊天入口 | 功能完整聊天客户端 | 底层 token 级交互 |
| **代码量** | ~1409 行 (单文件) | ~655 行 (+ server 复用) | ~1007 行 |
| **输入方法** | linenoise (POSIX) / getline (Win) | console::readline (高级/简单) | console::readline (高级/简单) |
| **颜色输出** | `LOG_COL_*` 宏直接 printf | `console::set_display()` 状态机 | `console::set_display()` + `LOG()` |
| **流式输出** | 逐 token printf + fflush | Server response reader | 逐 token LOG() |
| **推理架构** | 直接同步 `llama_decode()` 循环 | Server 线程 + 任务队列 | 直接同步循环 |
| **聊天模板** | `common_chat_templates` | `common_chat_templates` + reasoning | `common_chat_templates` |
| **历史浏览** | linenoiseHistory | `console::history_t` | 无 |
| **Tab 补全** | 无 | `auto_completion_callback` | 无 |
| **加载动画** | 无 | `console::spinner` | 无 |
| **Reasoning 显示** | 无 | 灰色 + `[Start/End thinking]` | 无 |
| **命令系统** | 仅 `/bye` | `/exit` `/clear` `/regen` `/image` `/audio` `/read` `/glob` | 无 |
| **上下文移位** | 无 | Server 自动处理 | 手动 `llama_memory_seq_*` |
| **会话存档** | 无 | 无 | `llama_state_save/load_file()` |
| **多模态** | 无 | 图片 + 音频 | 无 |
| **模型下载** | HF / Docker Hub (含进度条) | HF / Docker Hub | 无 |
| **Ctrl+C** | 直接 `exit(0)` | 双击退出 + 优雅中断 | 优雅中断 + 性能打印 |

### 选择建议

- **想快速对话**：用 `llama-run`，最简单，支持 HF 一键下载
- **需要完整功能**：用 `llama-cli`，支持多模态、reasoning、命令、补全
- **需要底层控制**：用 `llama-completion`，token 级交互、session 持久化、antiprompt
- **构建自定义工具**：根据复杂度选择参考蓝本
  - 简单工具：参考 `llama-run` 的直接 API 调用模式
  - 中等工具：参考 `llama-cli` 的 server 复用 + console 抽象模式
  - 底层工具：参考 `llama-completion` 的 token 级循环模式
