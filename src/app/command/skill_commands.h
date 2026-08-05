/**
 * @file skill_commands.h
 * @brief 磁盘 Skill 命令注册
 * @details 扫描 .claude/skills 目录，将技能注册为 PromptCommand
 *          （对应 example/cc 的 getSkillDirCommands）
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <string>

#include "agent/command/inclaude/registry.h"

namespace agent::command {

/// @brief 加载并注册项目/用户磁盘 skills
/// @param registry 命令注册表
/// @param cwd 当前工作目录（从该目录向上查找 .claude/skills）
void register_skill_commands(CommandRegistry& registry, const std::string& cwd);

} // namespace agent::command
