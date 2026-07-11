# Workx

> 一个现代化的终端 AI 聊天客户端，基于事件驱动架构，支持多 AI 提供商。

## 特性

- **多提供商支持**: 内置 OpenAI、Anthropic、DeepSeek 等预设，一键切换
- **SSE 流式响应**: 实时显示 AI 回复，无需等待完整响应
- **Markdown 渲染**: 支持表格、标题、列表、代码块、强调等完整 Markdown 语法
- **命令系统**: `/help`、`/exit`、`/clear`、`/regen`、`/model` 等内置命令
- **自定义模型**: 支持添加自定义模型和 API 端点
- **彩色输出**: 终端彩色渲染，语法高亮的代码块
- **输入历史**: 支持上下箭头浏览历史输入

## 架构概览

```
src/
├── agent/          # AI 后端接口层
│   ├── api/        # REST API 适配器、SSE 流读取、HTTP 客户端
│   ├── command/    # 命令执行器和注册表
│   ├── core/       # 聊天会话、查询引擎
│   └── model/      # 模型配置、提供商预设
├── app/            # 应用层
│   ├── command/    # 内置系统命令
│   ├── config/     # 配置管理、CLI 参数解析
│   └── ui/         # 模型选择器、路径补全
├── core/           # 核心基础设施
│   ├── config/     # 配置管理器
│   ├── events/     # 事件总线
│   └── task/       # 任务管理器
└── tui/            # 终端用户界面
    ├── core/       # 终端封装、屏幕管理、显示缓冲
    ├── input/      # 行编辑器、输入历史
    ├── render/     # 聊天渲染、Markdown 渲染、输出格式化
    ├── setup/      # 设置向导
    ├── utils/      # UTF-8 工具函数
    └── widgets/    # 状态栏、命令面板、底部栏
```

## 支持的 AI 提供商

| 提供商 | 预设名称 | 默认模型 |
|--------|----------|----------|
| OpenAI | `openai` | gpt-4o |
| Anthropic | `anthropic` | claude-sonnet-4-20250514 |
| DeepSeek | `deepseek` | deepseek-chat |
| Groq | `groq` | llama-3.3-70b-versatile |
| Together AI | `together` | mistralai/Mixtral-8x22B-Instruct-v0.1 |
| LM Studio | `lm-studio` | (自定义) |
| Custom URL | `openai-compatible` | (自定义) |

## 前置条件

- **CMake**: 3.21 或更高版本
- **C++ 编译器**: 支持 C++20（MSVC 14.5+ 或 GCC 10+）
- **vcpkg**: 依赖包管理器

## 构建步骤

> **注意**: 当前版本仅支持 Windows 平台。

### Windows (MSVC)

```bash
# 初始化 vcpkg（首次使用）
vcpkg install nlohmann-json catch2 curl

# 创建构建目录
mkdir build
cd build

# 配置 CMake（替换 [vcpkg_root] 为你的 vcpkg 安装路径）
cmake .. -DCMAKE_TOOLCHAIN_FILE=[vcpkg_root]/scripts/buildsystems/vcpkg.cmake

# 构建
cmake --build . --config Release
```

## 运行

```bash
# Windows
build\bin\workx.exe

# Linux/macOS
./build/bin/workx
```

首次运行时会启动设置向导，引导您配置 API Key 和选择默认模型。

## 命令参考

| 命令 | 说明 |
|------|------|
| `/help` | 显示所有可用命令 |
| `/exit` / `/quit` | 退出程序 |
| `/clear` | 清空聊天历史 |
| `/regen` | 重新生成上一条回复 |
| `/model` | 切换当前使用的模型 |

## 配置

配置文件位于用户目录下，包含 API Key、默认模型、超时设置等。设置向导会在首次运行时生成配置。

## 测试

```bash
# 运行单元测试
build/bin/workx_tests.exe
```

项目包含 100+ 个测试用例，覆盖核心功能模块。

## 许可证

MIT License
