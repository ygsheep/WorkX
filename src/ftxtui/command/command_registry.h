/**
 * @file command_registry.h
 * @brief FTXUI 独立的命令注册表（不依赖 agent）
 * @details 每个命令条目标注：命令、显示标题、可搜索关键词（中英文/别名），
 *          供命令面板做「标题 + 命令 + 关键词」引擎式搜索。
 */

#pragma once

#include <string>
#include <vector>

namespace ftxtui {

/// 命令面板条目
struct PaletteCommand {
    std::string command;   ///< 实际执行命令，如 "/model"
    std::string title;     ///< 显示标题，如 "切换模型"
    std::string keywords;  ///< 额外搜索关键词（中英文/别名），可空
};

/// FTXUI 独立命令注册表
class CommandRegistry {
public:
    /// 内置命令（中英文均可搜索，例如 "切换模型" / "switch model" 命中 /model）
    static CommandRegistry builtins();

    /// 注册一条命令（尾插，保持顺序）
    void add(PaletteCommand cmd);

    /// 全部命令（供命令面板填充，按注册顺序）
    const std::vector<PaletteCommand>& all() const { return entries_; }

private:
    std::vector<PaletteCommand> entries_;
};

}  // namespace ftxtui