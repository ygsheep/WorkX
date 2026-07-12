/**
 * @file file_read_tool.cpp
 * @brief FileReadTool 实现
 * @details 文件读取工具的具体实现
 * @version 1.0.0
 * @date 2026-07
 */

#include "agent/tool/FileReadTool/file_read_tool.h"

namespace agent::tool {

const std::string& FileReadTool::name() const {
    static const std::string n{"Read"};
    return n;
}

const std::string& FileReadTool::description() const {
    static const std::string d{"Reads a file from the local filesystem."};
    return d;
}

const std::string& FileReadTool::prompt() const {
    static const std::string p{
        "Reads a file from the local filesystem. "
        "Supports text, image, PDF, and notebook formats. "
        "Use offset/limit for partial reads."
    };
    return p;
}

nlohmann::json FileReadTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties", {
            {"file_path", {{"type", "string"}, {"description", "The absolute path to the file to read"}}},
            {"offset", {{"type", "integer"}, {"description", "The line number to start reading from (0-based)"}}},
            {"limit", {{"type", "integer"}, {"description", "Maximum number of lines to read"}}},
            {"pages", {{"type", "integer"}, {"description", "PDF page number to read"}}}
        }},
        {"required", {"file_path"}}
    };
}

ToolResult FileReadTool::call(
    const nlohmann::json& input,
    const ToolContext& ctx
) {
    // TODO: 实现文件读取逻辑
    return ToolResult::error("FileReadTool not implemented");
}

} // namespace agent::tool
