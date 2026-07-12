/**
 * @file file_edit_tool.cpp
 * @brief FileEditTool 实现
 * @details 文件编辑工具的具体实现
 * @version 1.0.0
 * @date 2026-07
 */

#include "agent/tool/FileEditTool/file_edit_tool.h"

namespace agent::tool {

const std::string& FileEditTool::name() const {
    static const std::string n{"Edit"};
    return n;
}

const std::string& FileEditTool::description() const {
    static const std::string d{"Performs exact string replacements in files."};
    return d;
}

const std::string& FileEditTool::prompt() const {
    static const std::string p{
        "Performs exact string replacements in files. "
        "old_string must match exactly once (or use replace_all)."
    };
    return p;
}

nlohmann::json FileEditTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties", {
            {"file_path", {{"type", "string"}, {"description", "The absolute path to the file to edit"}}},
            {"old_string", {{"type", "string"}, {"description", "The text to replace"}}},
            {"new_string", {{"type", "string"}, {"description", "The replacement text"}}},
            {"replace_all", {{"type", "boolean"}, {"description", "Replace all occurrences"}, {"default", false}}}
        }},
        {"required", {"file_path", "old_string", "new_string"}}
    };
}

ToolResult FileEditTool::call(
    const nlohmann::json& input,
    const ToolContext& ctx
) {
    // TODO: 实现文件编辑逻辑
    return ToolResult::error("FileEditTool not implemented");
}

} // namespace agent::tool
