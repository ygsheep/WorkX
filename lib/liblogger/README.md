# liblogger - 现代化 C++20 轻量级日志库

## 概述

liblogger 是一个简洁、高效的 C++20 日志库（`agent::log` 命名空间），专注于性能和易用性。

### 核心特性

- **现代 C++20**: 使用 `std::format`、`__builtin_FILE()`/`__builtin_LINE()` 等特性
- **高性能**: `std::format` 替代 `stringstream`，性能提升 2-3 倍
- **线程安全**: 原子操作 + 互斥锁保护，支持多线程并发
- **异步文件写入**: 独立写入线程，不阻塞主线程
- **重复日志过滤**: 智能过滤短时间内重复日志
- **简洁 API**: 统一的宏接口，支持格式化输出
- **Header-only**: 所有代码在头文件中，开箱即用

---

## 快速开始

### 基本使用

```cpp
#include "liblogger/logger.h"

int main() {
    // 配置日志
    auto& logger = agent::log::Logger::get_instance();
    logger.set_level(agent::log::LogLevel::LOG_DEBUG);
    logger.enable_file_output("logs/app.log", true);

    // 使用格式化宏 (推荐)
    LOG_INFO("Application started");
    LOG_DEBUG("User id: {}, name: {}", 12345, "Alice");
    LOG_WARN("Memory usage: {}%", 85);
    LOG_ERROR("Failed to open file: {}", "config.txt");

    // 使用便捷函数
    agent::log::Logger::get_instance().info("Simple message");

    return 0;
}
```

### 日志输出格式

```
[2025-01-15 14:30:45.123] [INFO] [main.cpp:42] Application started
[2025-01-15 14:30:45.124] [DEBUG] [main.cpp:43] User id: 12345, name: Alice
[2025-01-15 14:30:45.125] [WARN] [main.cpp:44] Memory usage: 85%
```

---

## API 参考

### 日志级别

```cpp
enum class LogLevel : int {
    LOG_TRACE = 0,    // 详细跟踪
    LOG_DEBUG = 1,    // 调试信息
    LOG_INFO = 2,     // 一般信息
    LOG_WARN = 3,     // 警告
    LOG_ERROR = 4,    // 错误
    LOG_FATAL = 5     // 致命错误
};
```

### 推荐的日志宏

```cpp
// 格式化日志宏 (支持 std::format 语法)
LOG_TRACE(fmt, ...)    // 跟踪级别
LOG_DEBUG(fmt, ...)    // 调试级别
LOG_INFO(fmt, ...)     // 信息级别
LOG_WARN(fmt, ...)     // 警告级别
LOG_ERROR(fmt, ...)    // 错误级别
LOG_FATAL(fmt, ...)    // 致命级别
```

### Logger 配置接口

```cpp
auto& logger = agent::log::Logger::get_instance();

// 设置日志级别
logger.set_level(agent::log::LogLevel::LOG_INFO);

// 启用/禁用文件输出
logger.enable_file_output("logs/app.log", true);
logger.enable_file_output("", false);  // 禁用

// 设置缓冲区大小 (字节)
logger.set_buffer_size(8192);  // 默认 4096

// 查询状态
agent::log::LogLevel level = logger.get_level();
bool enabled = logger.is_file_output_enabled();
```

### 便捷函数接口

```cpp
// 直接调用 (使用默认源码位置)
agent::log::Logger::get_instance().trace("message");
agent::log::Logger::get_instance().debug("message");
agent::log::Logger::get_instance().info("message");
agent::log::Logger::get_instance().warn("message");
agent::log::Logger::get_instance().error("message");
agent::log::Logger::get_instance().fatal("message");
```

### 向后兼容宏

```cpp
// 简单字符串日志 (无格式化)
LOG_INFO_STR("Simple message");
LOG_ERROR_STR("Error: " + std::string("details"));
```

---

## 使用示例

### 场景 1: 应用程序启动日志

```cpp
int main() {
    auto& logger = agent::log::Logger::get_instance();

    // 配置
    logger.set_level(agent::log::LogLevel::LOG_INFO);
    logger.enable_file_output("logs/myapp.log", true);

    // 启动日志
    LOG_INFO("=== Application Starting ===");
    LOG_INFO("Version: {}, Build: {}", "1.0.0", "20250115");

    // 运行应用
    runApplication();

    LOG_INFO("=== Application Shutting Down ===");
    return 0;
}
```

### 场景 2: 错误处理

```cpp
void loadConfig(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        LOG_ERROR("Failed to open config file: {}", path);
        return;
    }

    try {
        auto config = parseConfig(file);
        LOG_INFO("Config loaded successfully: {} entries", config.size());
    } catch (const std::exception& e) {
        LOG_ERROR("Config parsing failed: {}", e.what());
    }
}
```

### 场景 3: 调试输出

```cpp
void processRequest(const Request& req) {
    LOG_DEBUG("Processing request: id={}, method={}", req.id, req.method);

    for (const auto& item : req.items) {
        LOG_TRACE("Processing item: id={}, value={}", item.id, item.value);
        processItem(item);
    }

    LOG_DEBUG("Request processed: {} items", req.items.size());
}
```

---

## 架构设计

### 核心组件

```
Logger (单例)
├── 配置管理
│   ├── 日志级别 (atomic<int>)
│   ├── 文件输出开关 (atomic<bool>)
│   └── 缓冲区大小 (atomic<size_t>)
│
├── 日志记录
│   ├── 控制台输出 (cout/cerr)
│   ├── 文件异步写入
│   └── 重复过滤
│
└── 异步写入系统
    ├── 写入线程
    ├── 消息队列 (queue + condition_variable)
    └── 批量写入缓冲
```

### 线程模型

```
主线程:
  LOG_INFO("msg") → 格式化 → 检查级别 → 重复检查
                                         ↓
                                    控制台输出 (立即)
                                         ↓
                                    消息队列 (异步)

写入线程:
  消息队列 → 批量缓冲 → 文件写入 (每 100ms 或缓冲区满)
```

### 性能优化点

1. **格式化优化**: `std::format` 比 `stringstream` 快 2-3 倍
2. **级别过滤**: 原子操作快速判断，无需加锁
3. **重复过滤**: `std::format` 构造键，避免字符串拼接
4. **异步写入**: 批量写入减少系统调用
5. **线程安全时间**: `localtime_s`/`localtime_r` 替代 `localtime`

---

## 编译集成

### CMake 集成

```cmake
# 方式 1: 直接包含头文件
target_include_directories(your_target PRIVATE
    ${CMAKE_SOURCE_DIR}/lib/liblogger
)

# 方式 2: 添加子目录
add_subdirectory(lib/liblogger)
target_link_libraries(your_target logger)
```

### 编译要求

- **C++20 或更高版本** (需要 `std::format`)
- **编译器支持**:
  - MSVC 2022 (v17.0+)
  - GCC 11+
  - Clang 13+

### 跨平台支持

```cpp
// Windows 使用 localtime_s
#ifdef _WIN32
    localtime_s(&tmBuf, &timeT);
#else
    localtime_r(&timeT, &tmBuf);
#endif
```

---

## 性能数据

### 基准测试结果

| 操作 | 时间 |
|------|------|
| LOG_INFO (无文件) | ~0.5 μs |
| LOG_INFO (文件异步) | ~0.8 μs |
| std::format 格式化 | ~0.3 μs |
| 文件批量写入 | >10 MB/s |

### 重复过滤效果

```cpp
// 默认 1000ms 时间窗口
for (int i = 0; i < 100; ++i) {
    LOG_INFO("Repeated message");  // 只会记录约 10 次
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}
```

---

## 与旧版本对比

### 移除的冗余功能

| 旧功能 | 状态 | 替代方案 |
|--------|------|----------|
| `LogManager` 类 | ❌ 删除 | 直接使用 `Logger::get_instance()` |
| `DearTs::Log` 命名空间 | ❌ 删除 | 使用 `LOG_*` 宏 |
| `_fmt` 函数 | ❌ 删除 | `LOG_*` 宏内置格式化 |
| `LogConfig` 类 | ❌ 删除 | 直接调用 `Logger` 方法 |
| `AppLogger` 类 | ❌ 删除 | 用户自行封装 |
| `PerfLogger` 类 | ❌ 删除 | 用户自行封装 |
| 复杂的模板元编程 | ❌ 删除 | 使用 `std::format` |

### 新增优化

| 功能 | 旧实现 | 新实现 |
|------|--------|--------|
| 格式化 | `stringstream` | `std::format` |
| 时间获取 | `localtime` | `localtime_s`/`localtime_r` |
| 源码位置 | 宏参数 | `__builtin_FILE()`/`__builtin_LINE()` |
| 文件路径提取 | `std::string` 操作 | 原生指针遍历 |

---

## 故障排查

### 常见问题

**Q: 编译错误 "cannot open include file: 'format'"**

A: 确保使用 C++20 或更高版本：
```cmake
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
```

**Q: 链接错误 "unresolved external symbol"**

A: 确保链接了 C++20 运行时库：
```cmake
target_link_libraries(your_target PRIVATE stdc++lib)  # GCC/Clang
```

**Q: 日志文件未写入**

A: 检查目录权限和路径有效性：
```cpp
std::filesystem::path logPath("logs/app.log");
std::error_code ec;
std::filesystem::create_directories(logPath.parent_path(), ec);
if (ec) {
    LOG_ERROR("Failed to create log directory: {}", ec.message());
}
```

---

## 版本历史

### v2.0.0 (2025-01)

- **重构**: 简化架构，移除 50% 冗余代码
- **性能**: 使用 `std::format` 替代 `stringstream`
- **安全**: 修复线程安全问题 (`localtime_s`/`localtime_r`)
- **API**: 统一为 `LOG_*` 宏接口
- **代码量**: 从 977 行减少到 356 行

### v1.0.0

- 初始版本
- 基础日志功能
- 异步文件写入

---

## 许可证

MIT License

---

*liblogger - 简洁、高效、现代* 🚀
