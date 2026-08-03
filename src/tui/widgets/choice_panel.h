/**
 * @file choice_panel.h
 * @brief ChoicePanel — 多 Tab 单选/多选面板
 * @details 接受 JSON 配置，弹出 TUI 面板让用户选择，返回结构化结果。
 *          - ↑↓ 上下导航
 *          - ←→ 切换 Tab（环形）
 *          - 空格 勾选/取消（单选 Tab 内互斥）
 *          - Enter 确认当前 Tab 跳到下一个；最后一个 Tab 的 Enter 提交
 *          - Esc 取消整个面板
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <string>
#include <vector>
#include <optional>
#include <atomic>
#include <nlohmann/json.hpp>

namespace tui {

class Terminal;
class Screen;

/// @brief 选择项（对齐 cc AskUserQuestionTool 的 option）
struct ChoiceItem {
    std::string id;              ///< 标识（= label，自定义输入项为用户文本）
    std::string label;           ///< 显示文本
    std::string description;     ///< 选项说明（可选，来自 cc option.description）
    bool selected = false;
    bool is_custom_input = false;  ///< 是否为"自定义输入"特殊项
};

/// @brief Tab 分组（对应 cc 的一个 question）
struct ChoiceTab {
    std::string question;        ///< 完整问题文本（作为答案 map 的 key）
    std::string header;          ///< 短标签（chip，≤12 字符，作为 Tab 栏显示名）
    bool multi = false;          ///< true=多选, false=单选（空格互斥）
    bool allow_custom_input = true; ///< 是否在末尾添加"自定义输入"选项（默认 true）
    std::vector<ChoiceItem> items;
    int cursor = 0;              ///< 当前光标位置（0-based）
};

/// @brief 面板配置（从 JSON 解析，对齐 cc questions 结构）
struct ChoiceConfig {
    std::vector<ChoiceTab> tabs;  ///< 每个 tab 对应一个 question
};

/// @brief 面板结果
struct ChoiceResult {
    bool submitted = false;      ///< true=用户提交, false=用户取消
    /// @brief 答案映射: question → answer
    /// @details 单选: answer 为选中 label; 多选: answer 为逗号分隔的 label;
    ///          自定义输入: answer 为用户输入文本
    std::vector<std::pair<std::string, std::string>> answers;

    /// @brief 转为 JSON 字符串（供 ToolResult 返回给模型）
    /// @details 格式: {"status":"submitted"|"cancelled", "answers":{question: answer}}
    std::string to_json() const;
};

/// @brief 从 JSON 解析面板配置
/// @details 期望格式（对齐 cc AskUserQuestionTool）:
/// { "questions": [ { "question":"...", "header":"...",
///                    "multiSelect": false,
///                    "allow_custom_input": true,
///                    "options":[{"label":"...","description":"..."}] } ] }
/// @return 配置对象; 解析失败返回空 optional
std::optional<ChoiceConfig> parse_choice_config(const nlohmann::json& input);

/// @brief 运行选择面板（模态）
/// @param term Terminal 实例
/// @param scr Screen 差分渲染缓冲
/// @param config 面板配置
/// @param cancel_flag 外部取消标志（nullptr 表示不支持取消）
///                     工作线程超时后置位此标志并唤醒主循环，
///                     本函数收到 KEY_WAKE 后检查此标志，若已置位则返回 cancelled。
/// @return 用户选择结果（submitted=false 表示取消）
ChoiceResult run_choice_panel(Terminal* term, Screen* scr, const ChoiceConfig& config,
                               const std::atomic<bool>* cancel_flag = nullptr);

} // namespace tui
