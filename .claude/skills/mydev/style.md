# mydev:style — 代码风格

## 命名规范

| 类型 | 规范 | 示例 |
|------|------|------|
| 类/结构体 | PascalCase | `class PluginManager;` |
| 函数/方法 | snake_case | `void on_init();` |
| 成员变量 | m_ + snake_case | `std::string m_plugin_name;` |
| 局部变量 | snake_case | `int key_code;` |
| 常量 | UPPER_CASE | `const int MAX_SIZE = 1024;` |
| 命名空间 | PascalCase | `namespace Core::Event;` |
| 文件名 | snake_case | `application.cpp` |
| 接口类 | I 前缀 | `class IPlugin;` |

**访问器命名**：`get_xxx()`, `set_xxx()`, `is_xxx()`, `has_xxx()`

**事件/回调命名**：`on_xxx()`, `handle_xxx()`

## 格式化

- **缩进**：4 空格，不用制表符
- **括号**：K&R 风格（左括号不换行）
- **行长度**：建议 120，最长 150
- **操作符**：两侧空格 `a + b`
- **逗号后**：空格 `func(a, b)`
- **括号内**：无空格 `if (condition)`
- **函数调用**：括号前无空格 `func()`

```cpp
if (condition) {
    DoSomething();
} else if (other) {
    DoElse();
}

for (const auto& item : items) {
    Process(item);
}

switch (value) {
    case A: DoA(); break;
    case B: DoB(); break;
    default: DoDefault();
}

// 长参数换行
void LongFunction(
    const std::string& p1,
    int p2,
    const std::vector<T>& items
);
```

**.clang-format 要点**：`IndentWidth: 4`, `ColumnLimit: 120`, `BasedOnStyle: Google`

## 注释（Doxygen 风格）

```cpp
/** @file name.h
 *  @brief 简要说明
 *  @author Team
 *  @date YYYY-MM
 */

/** @brief 启动应用（阻塞直到退出）
 *  @param argc 参数数量
 *  @param argv 参数数组
 *  @return 退出码，0 成功
 */
int Run(int argc, char* argv[]);

/// @brief 获取窗口标题
std::string GetWindowTitle() const;
```

**原则**：
- 文件头必须包含 `@file`, `@brief`, `@date`
- 公共函数必须有 Doxygen 注释
- 解释"为什么"而非"是什么"
- 代码修改时同步更新注释
- TODO: `// TODO: 说明` / FIXME: `// FIXME: 说明 (#123)`

## 头文件组织

**包含顺序**（三段式，空行分隔）：
1. C++ 标准库 (`<memory>`, `<vector>`)
2. 第三方库 (`<SDL3/SDL.h>`, `<imgui.h>`)
3. 项目内部 (`"core/plugin/plugin.h"`)

**规则**：
- 统一使用 `#pragma once`
- 前向声明优先：`class AppState;` 代替 `#include "app_state.h"`
- 内联仅限短小 getter：`inline int get_width() const { return m_width; }`
