/**
 * @file skill_loader.h
 * @brief Skill 磁盘加载器
 * @details 扫描 .claude/skills/<name>/SKILL.md，解析并生成 PromptCommand
 *          （对应 example/cc 的 loadSkillsFromSkillsDir）
 * @version 1.0.0
 * @date 2026-08
 */

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "agent/command/inclaude/command.h"

namespace agent::skill {

/// @brief 从 cwd 向上到根收集存在的 .claude/skills 目录
/// @param cwd 起点目录
/// @return 目录列表，cwd 最深优先（越近优先级越高）
std::vector<std::string> find_skill_dirs_up_to_home(const std::string& cwd);

/// @brief 从给定基础目录加载 skills
/// @param base_dirs 如 ["<cwd>/.claude/skills", ...]
/// @return PromptCommand 列表（含别名命令；同一 SKILL.md 仅加载一次）
/// @note 无 SKILL.md 的子目录跳过；读取/解析失败不影响其它 skill
std::vector<std::shared_ptr<command::PromptCommand>> load_skills_from_dirs(
    const std::vector<std::string>& base_dirs);

} // namespace agent::skill
