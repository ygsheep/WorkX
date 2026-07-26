# 长期技术债登记（统一索引）

> 本文档整合 `PHASE3_LONG_TERM_REFACTOR.md`（详细方案）与历史登记，作为技术债的唯一索引。
> 详细重构方案请参考 `plan/PHASE3_LONG_TERM_REFACTOR.md`。
> 最后更新：2026-07-27（P2 C-2/C-3/C-4 配置系统 Schema 化完成）

## 状态总览

- 已修复：16 项（L-1 / L-2 / L-3 / L-6 / T-4 / T-6 / G-2 / G-3 / G-4 / D-4 / D-5 / D-6 / Q-1 / C-2 / C-3 / C-4）
- 已安全：1 项（T-5 PHASE3 判定无需修复）
- 待修复：13 项（L-5 / H-1 / H-2 / H-3 / H-4 / D-2 / D-3 / E-1 / E-5 / E-6 / Q-2 / Q-3 / Q-4）
- 文档化即可：1 项（Q-5）
- 详细方案：见 `PHASE3_LONG_TERM_REFACTOR.md`

---

## L 类：生命周期与裸指针

| 编号 | 现象 | 文件:行号 | 影响 | 状态 |
|------|------|-----------|------|------|
| L-1 | `IBackend* g_backend` 全局裸指针跨函数传递 | ~~main.cpp:170~~ | — | ✅ 已修复（改用 `session->backend()`） |
| L-2 | `ReActLoop::m_provider` 裸指针 + 默认移动构造 | ~~`react_loop.h:296, :192`~~ | — | ✅ 已修复（删除移动构造 + 构造 assert） |
| L-3 | `ChatRenderer::m_terminal` 裸指针无契约 | ~~`chat_renderer.h:66`~~ | — | ✅ 已修复（构造 assert 生命周期契约） |
| L-5 | `StreamSession::m_multi` 裸 CURLM 跨析构依赖 | `http_client.cpp:297, :201-211` | 低（防御已加） | ⬜ 未修复 |
| L-6 | `Client::m_task_manager` 裸指针移动后无 assert | ~~`client.h:196, client.cpp:136-151`~~ | — | ✅ 已修复（析构 assert） |

**说明**：
- L-1 采用 PHASE3 方案 A 已修复，`_scripts/fix_gbackend.py` 留存重构痕迹
- L-5 PHASE3 描述有偏差：`StreamSession` 类实际只在 `http_client.cpp` 内部定义（非 `sse_stream_reader.h`）
- L-6 PHASE3 本就判定"维持现状"，但建议的析构 assert 也未加

---

## H 类：HTTP 客户端健壮性

| 编号 | 现象 | 文件:行号 | 影响 | 状态 |
|------|------|-----------|------|------|
| H-1 | 无连接池（无 CURLSH 共享） | `http_client.cpp:311-322` | 中（HTTPS 握手开销） | ⬜ 未修复 |
| H-2 | 流式传输无总时长超时 | `http_client.cpp:180-189` | 中（异常慢响应无上限） | ⬜ 未修复 |
| H-3 | 重试逻辑分散（Client/ChatSession 两层） | `client.cpp:218-315, chat_session.cpp:226-342` | 中（策略不一致） | ⬜ 未修复 |
| H-4 | URL 解析双套逻辑（CURLU + fallback） | `http_client.cpp:29-80` | 低（维护成本） | ⬜ 未修复 |

---

## C 类：配置系统完善

| 编号 | 现象 | 文件:行号 | 影响 | 状态 |
|------|------|-----------|------|------|
| C-2 | 无结构化 Schema（无类型/范围/枚举） | ~~`config_manager.h:23-33`~~ | — | ✅ 已修复（ConfigSchema 结构 + 类型/范围/枚举校验，register_meta → register_schema 迁移 20 项配置） |
| C-3 | `ConfigScope` 内部仍用单例 | ~~`config_manager.h:91-123`~~ | — | ✅ 已修复（ConfigScope 构造注入 IConfigManager&，默认参数保留兼容） |
| C-4 | 环境变量硬编码分散，无 schema 绑定 | ~~`app_config.cpp:135-161`~~ | — | ✅ 已修复（6 个标准环境变量绑定 Schema.env_var 由 ConfigManager::load_from_env 统一加载；WORKX_NO_COLOR 保留 presence-only 语义；新增 `docs/ENVIRONMENT_VARIABLES.md` 集中文档） |

**说明**：
- C-2 在 `app_config.cpp::register_config_defaults()` 中将 20 项配置全部迁移到 `register_schema`，包含类型/范围/枚举约束
- C-3 ConfigScope 构造函数新增 `IConfigManager& cm` 参数（默认 `ConfigManager::instance()`），DI 路径已在 `test_config_manager.cpp` 测试
- C-4 `load_from_env()` 简化为 `cfg.load_from_env()` + WORKX_NO_COLOR 特殊处理；环境变量文档见 `docs/ENVIRONMENT_VARIABLES.md`

---

## G 类：日志系统治理

| 编号 | 现象 | 文件:行号 | 影响 | 状态 |
|------|------|-----------|------|------|
| G-2 | `~Logger()` detach 写线程，use-after-free 风险 | ~~`lib/liblogger/logger.h:141`~~ | — | ✅ 已修复（detach → join，先 join 再关闭文件流） |
| G-3 | 命名空间混乱（`Agent` vs `agent`） | ~~`logger.h:46, :344, :360-365`~~ | — | ✅ 已修复（namespace Agent → agent::log，保留 Agent alias 向后兼容） |
| G-4 | `get_instance()` 返回 `shared_ptr` 语义错误 | ~~`logger.h:162-168`~~ | — | ✅ 已修复（返回 Logger& 引用，Meyers Singleton，移除 static shared_ptr 成员） |

**额外发现**：`logger.h:344` 闭合花括号注释 `} // namespace DearTs` 与 `namespace Agent` 声明不匹配，是命名空间历史改名的旁证

---

## T 类：残余线程安全问题

| 编号 | 现象 | 文件:行号 | 影响 | 状态 |
|------|------|-----------|------|------|
| T-4 | `Terminal::m_running/m_initialized` 非 atomic | ~~`terminal.h:184-185`~~ | — | ✅ 已修复（改 atomic<bool>） |
| T-5 | `StreamingBuffer::stop()` flush 竞争 | `streaming_buffer.cpp:57-71` | — | ✅ 已安全（PHASE3 判定无需修复） |
| T-6 | `Task::m_start_time` 非 atomic + 无 static_assert | ~~`task_manager.h:146`~~ | — | ✅ 已修复（atomic<int64_t> 存 ns + static_assert + helper） |

**说明**：T-5 实现细节与 PHASE3 伪代码略有差异（用 `std::lock_guard` 而非 `atomic store`），但 PHASE3 §6.1 本就判定"当前实现安全"

---

## D 类：依赖注入深化

| 编号 | 现象 | 文件:行号 | 影响 | 状态 |
|------|------|-----------|------|------|
| D-2 | `main.cpp` 530 行手动组装，无工厂函数 | `src/app/main.cpp` | 中（维护成本） | ⬜ 未修复 |
| D-3 | `IBackend` 胖接口未拆分 `IBackendAdmin` | `i_backend.h:22-50` | 中（接口隔离原则） | ⬜ 未修复 |
| D-4 | Terminal/ChatRenderer/Client 依赖 `EventBus::instance()` | ~~`tui/core/terminal.cpp` 等 8 处~~ | — | ✅ 已修复（Terminal DI 三件套 + Client IEventBus* 注入 + ChatRenderer 复用 Terminal 路径） |
| D-5 | 工具内部直接读 `ConfigManager::instance()` | ~~`file_edit_tool.cpp` / `file_read_tool.cpp` 4 处~~ | — | ✅ 已修复（ToolContext 添加 config_manager_ptr + helper，ReActLoop 注入 IConfigManager*） |
| D-6 | `Terminal` 残留 `TaskManager::instance()` | ~~`terminal.cpp:98`~~ | — | ✅ 已修复（Terminal DI 三件套含 ITaskManager*，与 D-4 一并完成） |

**说明**：D-4/D-5/D-6 是 Phase 4 暂缓项，与 PHASE3 §7 D-2/D-3 编号有差异，本索引展开为独立项

---

## E 类：错误处理统一（V2 前置）

| 编号 | 现象 | 影响 | 状态 |
|------|------|------|------|
| E-1 | 4 种错误风格并存（异常/Result/optional/bool） | 中（V2 前置） | ⬜ 暂不实施（独立立项 V2） |
| E-5 | `ExecutionResult` 字段语义模糊 | 低 | ⬜ 未修复 |
| E-6 | `HttpResponse` 错误码与 HTTP 状态码混淆 | 低 | ⬜ 未修复 |

---

## Q 类：测试与 CI 质量

| 编号 | 现象 | 影响 | 状态 |
|------|------|------|------|
| Q-1 | MockConfigManager/MockEventBus/MockTaskManager 缺失 | ~~中（无法隔离测试）~~ | ✅ 已修复（`tests/unit/helpers/mock_*.h` 三个 Mock + `test_mock_helpers.cpp` 自测试） |
| Q-2 | 性能基准测试缺失 | 低 | ⬜ 未修复 |
| Q-3 | Linux 平台编译未验证 | 中（CI 风险） | ⬜ 未修复 |
| Q-4 | AutoTestServer 在 Linux 上未实测 | 中 | ⬜ 未修复 |
| Q-5 | LM Studio LLM 推理测试需手动启动 | 低 | ⬜ 文档化即可 |

**说明**：Q 类整合了旧 TECH_DEBT_REGISTRY.md 的 T-4/T-5/T-6/I-1/I-2，避免与 PHASE3 的 T 类（线程安全）编号冲突

---

## 优先级矩阵

| 优先级 | 项 | 理由 | 预估工期 |
|--------|-----|------|----------|
| **P0** | Q-3 + Q-4 | Linux CI 是后续所有验证的基础 | 2-3 天 |
| ~~**P1**~~ | ~~L-2 + L-3 + L-6~~ | ~~裸指针修复~~ | ✅ 已完成 |
| ~~**P1**~~ | ~~T-4 + T-6~~ | ~~防御性原子化 + static_assert~~ | ✅ 已完成 |
| ~~**P1**~~ | ~~G-2~~ | ~~Logger 析构 join~~ | ✅ 已完成 |
| ~~**P2**~~ | ~~D-4 + D-5 + D-6~~ | ~~DI 化补全（解锁 Mock 测试）~~ | ✅ 已完成 |
| ~~**P2**~~ | ~~Q-1~~ | ~~Mock 实现（依赖 D-4/D-5/D-6）~~ | ✅ 已完成 |
| ~~**P2**~~ | ~~C-2 + C-3 + C-4~~ | ~~配置系统 Schema 化~~ | ✅ 已完成 |
| ~~**P2**~~ | ~~G-3 + G-4~~ | ~~日志命名空间统一~~ | ✅ 已完成 |
| **P3** | H-1 + H-2 + H-3 + H-4 | HTTP 客户端演进 | 4 天 |
| **P3** | D-2 + D-3 | main.cpp 工厂 + IBackendAdmin 拆分 | 2 天 |
| **P3** | E-5 + E-6 | 错误处理类型安全 | 2 天 |
| **P3** | Q-2 + Q-5 | 性能基准 + 文档 | 2 天 |
| **V2** | E-1 | 错误处理统一（独立立项） | — |

**总预估**：约 4-6 周（与 PHASE3 §9 路线图一致），可与业务需求并行推进

---

## 关联文档

- `plan/PHASE3_LONG_TERM_REFACTOR.md` — 详细重构方案（6 周路线图）
- `plan/ARCH_ANALYSIS_REPORT.md` — 架构分析报告（债务诊断依据）
- `plan/ARCH_REFACTOR_PLAN.md` — Phase 1-6 已完成的重构记录
- `plan/TASKS.md` — 可执行任务清单（按优先级排序）
