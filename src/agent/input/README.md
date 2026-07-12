# Input — 用户输入处理架构

## 三层架构

```
回车
  │ raw text
  ▼
┌──────────────────────────────┐
│        InputParser           │  ← 解析层
│  parse(raw_text)             │     纯函数，无副作用
│    → ParsedInput             │     raw string → 结构化类型
└──────────┬───────────────────┘
           │ ParsedInput
           ▼
┌──────────────────────────────┐
│       InputProcessor         │  ← 处理层
│  process(parsed, ctx)        │     编排调度
│    → ProcessResult           │     根据 type 分派到对应执行器
│  ┌──────────┐                │
│  │ dispatch │                │
│  │ by type  │                │
│  └──┬───┬───┘                │
│     │   │                    │
└─────┼───┼────────────────────┘
      │   │
      │   └──────────┐
      │              │
      ▼              ▼
  SlashCommand    Text / BashCommand
      │              │
      ▼              ▼
┌─────────────┐  ┌──────────────────┐
│CommandExec. │  │  ChatSession     │  ← 执行层
│ .execute()  │  │  TaskManager     │     真正干活的组件
│             │  │  EventBus        │
│ 同步执行     │  │                  │
│ 结果→EventBus│  │ 异步 LLM / Bash  │
└─────────────┘  │ 结果→EventBus     │
                 └──────────────────┘
```

## 处理管道

```
Terminal (统一 UserInputEvent)
  → main.cpp subscriber
    → InputProcessor::process(raw_text)     ← 处理层
      → InputParser::parse()                ← 解析层（含 @ 提取）
      → CommandExecutor / ChatSession       ← 执行层
    → ProcessResult 驱动下一步
      ├─ output_text   → EventBus::publish
      └─ should_query  → ChatSession::send_message()
```

## TODO

- [x] `InputParser` — 解析层（含 @ 引用提取）
- [x] `InputProcessor` — 处理层骨架（含 stub BashCommand）
- [x] main.cpp 用 `InputProcessor` 替换直接 `CommandExecutor`
- [x] 移除 `CommandEvent`，`Terminal` 统一发布 `UserInputEvent`
- [x] `ChatSession` 移除内部事件订阅，改为 `send_message()` 显式调用
- [ ] `InputProcessor::process_bash_command` 接入 `TaskManager`
