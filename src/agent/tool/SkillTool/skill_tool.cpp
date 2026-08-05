/**
 * @file skill_tool.cpp
 * @brief SkillTool 实现
 * @details 通过 CommandRegistry 按名称查找 PromptCommand，展开其提示词。
 *          未找到时返回 ResourceNotFound 错误。
 * @version 1.0.0
 * @date 2026-08
 */

#include "agent/tool/SkillTool/skill_tool.h"

#include "agent/command/inclaude/command.h"
#include "core/utils/error.h"

namespace agent::tool {

SkillTool::SkillTool(std::shared_ptr<command::CommandRegistry> registry)
    : registry_(std::move(registry)) {}

void SkillTool::set_registry(std::shared_ptr<command::CommandRegistry> registry) {
    std::lock_guard<std::mutex> lock(m_mutex);
    registry_ = std::move(registry);
}

const std::string& SkillTool::name() const {
    static const std::string n{"Skill"};
    return n;
}

const std::string& SkillTool::description() const {
    static const std::string d{"Loads a skill by name and returns its full instructions."};
    return d;
}

const std::string& SkillTool::prompt() const {
    static const std::string p{
        "Loads the detailed instructions of a skill by its name. "
        "Use when the current task matches a skill's description or when_to_use."
    };
    return p;
}

nlohmann::json SkillTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties", {
            {"name", {{"type", "string"}, {"description", "The skill name to load"}}}
        }},
        {"required", {"name"}},
        {"additionalProperties", false}
    };
}

ResultV2<ToolResult> SkillTool::call(
    const nlohmann::json& input,
    const ToolContext& ctx
) const {
    const auto name_it = input.find("name");
    if (name_it == input.end() || !name_it->is_string() || name_it->get<std::string>().empty()) {
        return ResultV2<ToolResult>::err(
            Error::Code::InvalidInput,
            "Missing required argument: name");
    }

    std::shared_ptr<command::CommandRegistry> registry;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        registry = registry_;
    }
    if (!registry) {
        return ResultV2<ToolResult>::err(
            Error::Code::InvalidInput,
            "Skill registry not configured");
    }

    const auto cmd = registry->find_by_name(name_it->get<std::string>());
    if (!cmd) {
        return ResultV2<ToolResult>::err(
            Error::Code::ResourceNotFound,
            "Skill not found: " + name_it->get<std::string>());
    }

    auto* prompt_cmd = dynamic_cast<command::PromptCommand*>(cmd.get());
    if (!prompt_cmd) {
        return ResultV2<ToolResult>::err(
            Error::Code::InvalidInput,
            "Not a skill: " + name_it->get<std::string>());
    }

    command::CommandContext cctx;
    cctx.cwd = ctx.cwd;
    cctx.model = ctx.model;
    cctx.session_id = ctx.session_id;

    std::string text;
    for (const auto& block : prompt_cmd->generate_prompt("", cctx)) {
        if (block.type != command::PromptBlockType::Text) continue;
        if (!text.empty()) text += "\n";
        text += block.text;
    }
    return ResultV2<ToolResult>::ok(ToolResult::ok(std::move(text)));
}

} // namespace agent::tool
