/**
 * @file file_write_tool.cpp
 * @brief FileWriteTool 实现
 * @details 文件写入工具的具体实现
 * @version 1.0.0
 * @date 2026-07
 */

#include "agent/tool/FileWriteTool/file_write_tool.h"

namespace agent::tool {

const std::string& FileWriteTool::name() const {
    static const std::string n{"Write"};
    return n;
}

const std::string& FileWriteTool::description() const {
    static const std::string d{"Writes content to a file, creating or overwriting it."};
    return d;
}

const std::string& FileWriteTool::prompt() const {
    static const std::string p{
        "Writes content to a file. "
        "Creates new files or overwrites existing ones. "
        "Generates diff for updates."
    };
    return p;
}

nlohmann::json FileWriteTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties", {
            {"file_path", {{"type", "string"}, {"description", "The absolute path to the file to write"}}},
            {"content", {{"type", "string"}, {"description", "The content to write to the file"}}}
        }},
        {"required", {"file_path", "content"}}
    };
}

ToolResult FileWriteTool::call(
    const nlohmann::json& input,
    const ToolContext& ctx
) {
    // TODO: 实现文件写入逻辑
    return ToolResult::error("FileWriteTool not implemented");
}

} // namespace agent::tool
