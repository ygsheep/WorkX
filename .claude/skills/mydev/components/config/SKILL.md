---
name: mydev:config
description: ConfigManager + ConfigScope type-safe configuration management for C++20. Use when needing ConfigManager, ConfigScope, configuration, settings, validation callbacks, or JSON config persistence.
---

# ConfigManager + ConfigScope — 配置管理

类型安全的分层配置，验证/变更回调，JSON 持久化。

## 快速参考

```cpp
#include "config_manager.h"

// 设置/获取
ConfigManager::instance().set("app.window.width", 1280);
auto w = ConfigManager::instance().get<int>("app.window.width");
auto w = ConfigManager::instance().get_or<int>("app.window.width", 800);
ConfigManager::instance().has("app.window.width");
ConfigManager::instance().remove("app.window.width");

// 元数据（验证 + 变更回调）
ConfigManager::instance().register_meta("app.window.width", {
    .description = "Window width",
    .default_value = 1280,
    .is_required = false,
    .validate_callback = [](const ConfigValue& v) -> Result<void, std::string> {
        int w = std::get<int>(v);
        if (w < 100 || w > 4096)
            return Result<void, std::string>::err("Out of range");
        return Result<void, std::string>::ok();
    },
    .change_callback = [](const ConfigValue& v) {
        // 窗口宽度变更时更新
    }
});

// 全局变更回调
ConfigManager::instance().add_change_callback(
    [](const std::string& key, const ConfigValue& old_val, const ConfigValue& new_val) {
        // 所有配置变更时触发
    }
);

// 持久化（JSON 嵌套格式：点分键自动展开为嵌套对象）
// save_to_file 写出：{"app": {"window": {"width": 1280}}}
// load_from_file 同时兼容嵌套格式和旧的扁平格式 {"app.window.width": 1280}
ConfigManager::instance().load_from_file("config.json");
ConfigManager::instance().save_to_file("config.json");

// RAII 作用域（自动前缀隔离）
ConfigScope scope("plugin.chat");
scope.set("model", std::string("gpt-4"));  // 实际 key: "plugin.chat.model"
auto m = scope.get_or<std::string>("model", "default");
```

## 支持的值类型

```cpp
using ConfigValue = std::variant<bool, int, double, std::string>;
```

## 设计决策

| 决策 | 原因 |
|------|------|
| `variant` 值 | 编译期类型安全，避免 JSON 依赖 |
| 点号分隔键 | `app.window.width` 语义清晰，JSON 持久化时自动展开为嵌套对象 |
| 锁外回调 | 防死锁（回调中可能操作 ConfigManager） |
| `get_or` | 最常用获取模式，简化代码 |
| `ConfigScope` RAII | 插件卸载时自动清理 |

## 依赖

- [mydev:result](../result/) — 必需，错误处理
- nlohmann/json — 持久化时需要（`load_from_file` / `save_to_file`）

## 源码

- [src/config_manager.h](src/config_manager.h) — 头文件
- [src/config_manager.cpp](src/config_manager.cpp) — 实现文件
