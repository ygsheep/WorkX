# mydev:quality — 代码质量

## 单元测试命名

```cpp
// 格式：MethodName_Condition_ExpectedResult
TEST_F(MyTest, OnInit_ValidConfig_ReturnsSuccess) {
    auto result = app.Initialize(validConfig);
    EXPECT_EQ(result, SDL_APP_SUCCESS);
}
```

## 审查清单

1. **内存安全** — 无泄漏，智能指针正确
2. **线程安全** — 共享数据有同步
3. **错误处理** — 所有路径有处理
4. **性能** — 不引入回归
5. **可读性** — 清晰，注释充分

## 性能优化习惯

- **批处理**：减少 draw call / IO 操作
- **缓存友好**：连续内存布局，排序减少状态切换
- **预分配**：`m_items.reserve(count * 2)`
- **RAII 计时器**：`ScopedTimer timer("name");` / `PROFILE_FUNCTION()`
- **多线程**：小任务(<1000项)直接串行，避免调度开销
