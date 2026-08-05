/**
 * @file skill_commands.cpp
 * @brief 磁盘 Skill 命令注册实现
 * @version 1.0.0
 * @date 2026-08
 */

#include "app/command/skill_commands.h"

#include "agent/skill/inclaude/skill_loader.h"

namespace agent::command {

void register_skill_commands(CommandRegistry& registry, const std::string& cwd) {
    auto base_dirs = skill::find_skill_dirs_up_to_home(cwd);
    // 用户级目录（~/.claude/skills、~/.workx/skills）优先级低于项目链，
    // 同名命令由 load_skills_from_dirs 去重保留首个（项目覆盖用户）
    for (const auto& dir : skill::find_user_skill_dirs()) {
        base_dirs.push_back(dir);
    }
    const auto skills = skill::load_skills_from_dirs(base_dirs);
    for (const auto& skill : skills) {
        registry.register_command(skill);
    }
}

} // namespace agent::command
