# mydev:cpp — C++ 规范

## 智能指针

| 类型 | 用途 |
|------|------|
| `std::unique_ptr` | 独占所有权（首选） |
| `std::shared_ptr` | 共享所有权 |
| `std::weak_ptr` | 观察指针，避免循环引用 |

```cpp
std::unique_ptr<AppState> m_app_state;
std::vector<std::shared_ptr<IApplicationLayer>> m_layers;
std::weak_ptr<Application> m_app;  // 防止循环引用
```

## RAII 模式（核心习惯）

```cpp
// 通用 RAII 包装示例
class ScopedMutex {
    ScopedMutex() : m_mutex(CreateMutex()) {}
    ~ScopedMutex() { if (m_mutex) DestroyMutex(m_mutex); }
    ScopedMutex(const ScopedMutex&) = delete;
    ScopedMutex(ScopedMutex&&) noexcept;
};
```

**习惯**：所有资源获取必须在析构函数中释放，删除拷贝构造，提供移动语义。

## Lambda

```cpp
auto cb = [this](const Event& e) { handle(e); };         // 事件回调
auto proc = [data = std::move(data)]() mutable { /* */ }; // 移动捕获
```

## 现代特性

- **auto**：仅复杂类型 `auto it = find_if(...)`，不用 `auto x = 5;`
- **范围 for**：遍历首选 `for (const auto& x : items)`
- **constexpr**：编译时常量 `constexpr int MAX = 1024;`
- **结构化绑定**：`auto [ok, data] = Parse();`
- **移动语义**：`v.push_back(std::move(obj));`
- **Ranges (C++20)**：惰性求值管道
  ```cpp
  auto e = items | std::views::filter(&T::is_valid) | std::views::transform(&T::get_name);
  ```
  ⚠️ Views 惰性求值，避免调 `size()`；调试时用范围 for

## 错误处理

**Result<T, E>（首选，不用异常做控制流）**：
```cpp
Result<void, std::string> on_load() {
    if (!init()) return Result<void, std::string>::err("失败");
    return Result<void, std::string>::ok();
}
auto r = func();
if (r.isOk()) use(r.unwrap()); else LOG_ERROR("{}", r.error());
```

> 💡 Result\<T, E\> 的完整实现和使用模式见 [components/result.md](components/result.md)

**断言（仅 Debug 不变量，不用于可恢复错误）**：
```cpp
assert(m_renderer && "Must be initialized");
static_assert(SDL_MAJOR_VERSION >= 3);  // 编译时
```

**日志分级**：
`LOG_TRACE / LOG_DEBUG / LOG_INFO / LOG_WARN / LOG_ERROR / LOG_FATAL`

## 内存管理

- **容器优化**：连续内存 `std::vector<Transform>`；`reserve()` 预分配
- **小对象**：值存储 `vector<Transform>` 不用 `vector<Transform*>`
- **RAII 锁**：`MutexLock lock(mutex);`（RAII）— 不用 `mutex.lock()`/`unlock()`
- **智能指针 custom deleter**：`unique_ptr<T, decltype(&DestroyFunc)>`
