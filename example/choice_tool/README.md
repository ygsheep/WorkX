# ChoiceTool / AskUser 示例

AskUser 是一个阻塞式 TUI 选择面板工具，模型通过 tool_use 调用，弹出交互面板让用户选择，
**阻塞等待用户响应**，结果返回给模型。支持超时机制，时间到后自动返回 `timeout` 状态。

## 输入格式（对齐 cc AskUserQuestionTool）

```json
{
  "questions": [
    {
      "question": "完整的问题文本？",       // 必填，作为答案 map 的 key
      "header": "短标签",                   // 必填，Tab 栏显示名（≤12 字符）
      "multiSelect": false,                 // 可选，默认 false；true=多选，false=单选（空格互斥）
      "allow_custom_input": true,           // 可选，默认 true；true=末尾追加 ✎ 自定义输入 选项
      "options": [                          // 必填，2-4 个选项
        { "label": "选项1", "description": "说明文字（可选）" },
        { "label": "选项2" }
      ]
    }
  ],
  "timeout_ms": 300000                      // 可选，默认 300000(5分钟)；0=不限时；超时自动返回 timeout
}
```

## 示例文件

| 文件 | 场景 |
|------|------|
| `01_single_select.json`    | 单问题单选：选择修复策略（allow_custom_input 默认 true） |
| `02_multi_select.json`     | 单问题多选：选择要修改的文件（allow_custom_input=false） |
| `03_multi_tab.json`        | 多问题混合：策略(单选) + 文件(多选) + 测试(多选，关闭自定义) |
| `04_result_submitted.json` | 返回结果：用户提交 |
| `05_result_cancelled.json` | 返回结果：用户按 Esc 取消 |
| `06_result_timeout.json`   | 返回结果：超时未响应 |

## 交互流程

```
╭─────────────────────────────────────────────────────────╮
│  选择哪种修复策略？                                       │  ← 当前问题的完整文本
├─── ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─┤
│   策略   文件   测试                                      │  ← Tab 栏（当前: 策略，蓝底）
├─── ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─┤
│  ❯ [√] 1、重构                                           │  ← 光标行（空格勾选，单选互斥）
│    [ ] 2、打补丁                                         │
│    [ ] 3、重写                                           │
│    [ ] 4、✎ 自定义输入...                                 │  ← allow_custom_input=true 时追加
├─────────────────────────────────────────────────────────┤
│  ↑↓ 导航  ←→ 切换 Tab  空格 勾选(单选)  Enter 确认  Esc 取消 │  ← 提示行
╰─────────────────────────────────────────────────────────╯
```

- **↑↓**：上下移动光标；移到 ✎ 自定义输入 项时自动进入输入模式
- **←→**：切换问题（环形）
- **空格**：勾选/取消当前项（单选问题内互斥）；在自定义项上进入输入模式
- **Enter**：确认当前问题并跳到下一个；最后一个问题的 Enter 提交全部
- **Esc**：取消整个面板（输入模式下仅退出输入模式）

### 自定义输入

- 选中 ✎ 自定义输入 项后自动进入输入模式，显示 `buffer█`
- 输入模式下：可打印字符追加、Backspace 删除（UTF-8 安全）、Enter 确认、Esc 取消
- 上下/左右键移动前会自动提交非空输入并选中当前项

## 返回格式

### 用户提交

```json
{
  "status": "submitted",
  "answers": {
    "选择哪种修复策略？": "重构",
    "选择要修改的文件？": "src/app/main.cpp,src/app/factory.cpp",
    "需要运行哪些验证？": "单元测试"
  }
}
```

- 单选：answer 为选中项的 label 字符串
- 多选：answer 为逗号分隔的 label 字符串
- 自定义输入：answer 为用户输入的文本

### 用户取消

```json
{ "status": "cancelled", "message": "User cancelled the question" }
```

### 超时未响应

```json
{ "status": "timeout", "message": "User did not respond within 300s" }
```

## 预览程序

```bash
# 使用默认示例（多问题混合）
./build/bin/Release/choice_preview.exe

# 加载指定 JSON 文件
./build/bin/Release/choice_preview.exe example/choice_tool/01_single_select.json
```

操作完成后结果 JSON 输出到 stdout。
