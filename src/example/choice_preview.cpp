/**
 * @file choice_preview.cpp
 * @brief ChoiceTool TUI 预览程序
 * @details 独立可执行文件，用于预览 ChoicePanel 的 TUI 效果。
 *          用法：
 *            choice_preview                  # 默认加载多 Tab 混合示例
 *            choice_preview <json_file>      # 加载指定 JSON 文件
 *
 *          启动后弹出选择面板，用户操作完成后输出结果 JSON 到 stdout。
 * @version 1.0.0
 * @date 2026-08
 */

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include <nlohmann/json.hpp>

#include "core/events/event_bus.h"
#include "core/config/config_manager.h"
#include "core/task/task_manager.h"
#include "tui/core/terminal.h"
#include "tui/core/screen.h"
#include "tui/widgets/choice_panel.h"
#include "app/factory.h"

namespace {

/// @brief 默认示例配置（多问题混合：策略 + 文件 + 测试）
/// @details 对齐 AskUserTool 的 schema：questions 数组，每项含
///          question(完整问题) / header(短标签≤12字符) / multiSelect / options
nlohmann::json default_example() {
    return R"({
        "questions": [
            {
                "question": "选择哪种修复策略？",
                "header": "策略",
                "multiSelect": false,
                "options": [
                    {"label": "重构", "description": "提取公共函数，消除重复代码"},
                    {"label": "打补丁", "description": "最小改动，仅修复当前 bug"},
                    {"label": "重写", "description": "整体重写该模块"}
                ]
            },
            {
                "question": "选择要修改的文件？",
                "header": "文件",
                "multiSelect": true,
                "options": [
                    {"label": "src/app/main.cpp"},
                    {"label": "src/agent/core/chat_session.h"},
                    {"label": "src/agent/core/chat_session.cpp"},
                    {"label": "src/app/factory.cpp"}
                ]
            },
            {
                "question": "需要运行哪些验证？",
                "header": "测试",
                "multiSelect": true,
                "allow_custom_input": false,
                "options": [
                    {"label": "单元测试"},
                    {"label": "集成测试"},
                    {"label": "手动验证"}
                ]
            }
        ]
    })"_json;
}

/// @brief 从文件加载 JSON
nlohmann::json load_from_file(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs) {
        throw std::runtime_error("无法打开文件: " + path);
    }
    std::stringstream ss;
    ss << ifs.rdbuf();
    return nlohmann::json::parse(ss.str());
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        // 1. 加载配置
        nlohmann::json input_json;
        if (argc >= 2) {
            input_json = load_from_file(argv[1]);
            std::cerr << "[choice_preview] 已加载: " << argv[1] << "\n";
        } else {
            input_json = default_example();
            std::cerr << "[choice_preview] 使用默认示例（多 Tab 混合）\n";
            std::cerr << "[choice_preview] 提示: choice_preview <json_file> 可加载自定义配置\n";
        }

        // 2. 解析为 ChoiceConfig
        auto config = tui::parse_choice_config(input_json);
        if (!config) {
            std::cerr << "[choice_preview] 配置解析失败\n";
            return 1;
        }

        // 3. 初始化 Terminal（复用 workx 主程序的初始化流程）
        tui::Terminal terminal(
            &agent::EventBus::instance(),
            &agent::ConfigManager::instance(),
            &agent::TaskManager::instance(),
            agent::make_terminal_config(agent::ConfigManager::instance()));

        auto init_result = terminal.initialize();
        if (init_result.isErr()) {
            std::cerr << "[choice_preview] Terminal 初始化失败: " << init_result.error() << "\n";
            terminal.restore();
            return 1;
        }

        tui::Screen screen(&terminal);

        // 4. 运行选择面板
        std::cerr << "[choice_preview] 启动选择面板，请操作...\n\n";
        tui::ChoiceResult result = tui::run_choice_panel(&terminal, &screen, *config);

        // 5. 恢复终端
        terminal.restore();

        // 6. 输出结果到 stdout
        std::cout << "\n========== 选择结果 ==========\n";
        std::cout << result.to_json() << "\n";
        std::cout << "==============================\n";

        if (result.submitted) {
            std::cerr << "\n[choice_preview] 用户已提交\n";
        } else {
            std::cerr << "\n[choice_preview] 用户已取消\n";
        }

        return result.submitted ? 0 : 1;
    } catch (const std::exception& e) {
        std::cerr << "[choice_preview] 异常: " << e.what() << "\n";
        return 2;
    }
}
