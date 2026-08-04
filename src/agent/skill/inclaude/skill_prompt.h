/**
 * @file skill_prompt.h
 * @brief Skills 列表 → system prompt 小节
 * @details 生成 "Available skills:" 段落（name + description + when_to_use），
 *          对应 example/cc 的 getSlashCommandToolSkills 过滤与注入。
 *          纯函数，header-only。
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <string>

#include "agent/command/inclaude/command.h"
#include "agent/command/inclaude/registry.h"

namespace agent::skill {

/// @brief 构建 skills 提示词小节
/// @param registry 命令注册表
/// @return 形如 "\n\n# Available skills\n- <name>: <description>..." 的文本；
///         无模型可调用的 skill 时返回空串
inline std::string build_skills_prompt_section(const command::CommandRegistry& registry) {
    std::string out;
    for (const auto& cmd : registry.get_by_type("prompt")) {
        if (cmd->loaded_from() != command::LoadSource::Skills) continue;
        if (cmd->is_model_invocation_disabled()) continue;
        if (cmd->description().empty() && !cmd->when_to_use().has_value()) continue;

        out += "- ";
        out += cmd->name();
        out += ": ";
        if (!cmd->description().empty()) {
            out += cmd->description();
            if (cmd->when_to_use().has_value()) {
                out += " (when to use: " + cmd->when_to_use().value() + ")";
            }
        } else {
            out += cmd->when_to_use().value();
        }
        out += "\n";
    }
    if (out.empty()) return std::string{};
    return "\n\n# Available skills\nUse the Skill tool to load the full instructions of a skill.\n" + out;
}

} // namespace agent::skill
