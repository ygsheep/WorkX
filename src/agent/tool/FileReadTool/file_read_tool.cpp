/**
 * @file file_read_tool.cpp
 * @brief FileReadTool 实现
 * @details 文件读取工具的具体实现：路径解析、编码检测、行号格式化、offset/limit 流式读取。
 *          所有 filesystem 操作使用 std::error_code 重载，避免异常抛出。
 * @author workx
 * @version 1.3.0
 * @date 2026-07
 */

#include "agent/tool/FileReadTool/file_read_tool.h"
#include "agent/tool/constants.h"
#include "agent/tool/encoding.h"
#include "core/config/config_manager.h"
#include "app/config/app_config.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <vector>

namespace agent::tool {

namespace fs = std::filesystem;

using namespace agent::tool::constants;

// ============================================================
// 元数据方法
// ============================================================

const std::string& FileReadTool::name() const {
    static const std::string n{"Read"};
    return n;
}

const std::string& FileReadTool::description() const {
    static const std::string d{"Reads a file from the local filesystem."};
    return d;
}

const std::string& FileReadTool::prompt() const {
    static const std::string p = std::format(
        "Reads a file from the local filesystem. "
        "By default, it reads up to {} lines starting from the beginning of the file. "
        "You can optionally specify a line offset and limit (especially handy for long files), "
        "but it's recommended to read the whole file by not providing these parameters. "
        "When you already know which part of the file you need, only read that part. "
        "This can be important for larger files. "
        "Files larger than {} bytes will return an error; use offset and limit for larger files. "
        "The file_path parameter must be an absolute path, not a relative path. "
        "Assume this tool is able to read all files on the machine. "
        "If the User provides a path to a file assume that path is valid. "
        "It is okay to read a file that does not exist; an error will be returned.",
        MAX_LINES_TO_READ, MAX_FILE_SIZE_BYTES
    );
    return p;
}

nlohmann::json FileReadTool::input_schema() const {
    return {
        {"type", "object"},
        {"properties", {
            {"file_path", {
                {"type", "string"},
                {"description", "The absolute path to the file to read"}
            }},
            {"offset", {
                {"type", "integer"},
                {"minimum", 1},
                {"description", "The line number to start reading from. Only provide if the file is too large to read at once"}
            }},
            {"limit", {
                {"type", "integer"},
                {"minimum", 1},
                {"description", "The number of lines to read. Only provide if the file is too large to read at once."}
            }},
            {"pages", {
                {"type", "string"},
                {"description", "Page range for PDF files (e.g., \"1-5\", \"3\", \"10-20\"). Only applicable to PDF files. Maximum 20 pages per request."}
            }}
        }},
        {"required", {"file_path"}},
        {"additionalProperties", false}
    };
}

// ============================================================
// 输入验证
// ============================================================

ValidationResult FileReadTool::validate_input(
    const nlohmann::json& input,
    const ToolContext& /*ctx*/
) const {
    // 检查必填字段 file_path
    if (!input.contains("file_path") || !input["file_path"].is_string()) {
        return ValidationResult::err("Missing required field: file_path");
    }
    if (input["file_path"].get<std::string>().empty()) {
        return ValidationResult::err("file_path must not be empty");
    }
    // 可选字段 offset：必须为正整数（1-based 行号）
    if (input.contains("offset")) {
        if (!input["offset"].is_number_integer()) {
            return ValidationResult::err("offset must be an integer");
        }
        if (input["offset"].get<int>() < 1) {
            return ValidationResult::err("offset must be >= 1");
        }
    }
    // 可选字段 limit：必须为正整数
    if (input.contains("limit")) {
        if (!input["limit"].is_number_integer()) {
            return ValidationResult::err("limit must be an integer");
        }
        if (input["limit"].get<int>() <= 0) {
            return ValidationResult::err("limit must be > 0");
        }
    }
    return ValidationResult::ok();
}

// ============================================================
// 私有辅助方法
// ============================================================

std::string FileReadTool::format_with_line_numbers(
    const std::vector<std::string>& lines,
    int start_line
) {
    if (lines.empty()) return "";

    const int last_line = start_line + static_cast<int>(lines.size());
    // 计算行号宽度（按最大行号的位数）
    int width = 1;
    for (int n = last_line; n >= 10; n /= 10) ++width;

    std::string result;
    result.reserve(lines.size() * 80);

    for (size_t i = 0; i < lines.size(); ++i) {
        const int line_num = start_line + static_cast<int>(i);
        // 右对齐行号 + → (U+2192) + 内容
        std::string num_str = std::to_string(line_num);
        if (static_cast<int>(num_str.size()) < width) {
            result.append(width - num_str.size(), ' ');
        }
        result += num_str;
        result += "\xe2\x86\x92";  // → (U+2192) UTF-8 编码
        result += lines[i];
        result += '\n';
    }

    // 移除末尾多余换行，保持输出整洁
    if (!result.empty()) {
        result.pop_back();
    }
    return result;
}

ToolResult FileReadTool::read_directory(const fs::path& dir_path) {
    std::vector<std::string> entries;

    // 使用 skip_permission_denied 跳过无权限目录，遇错不中断
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir_path, fs::directory_options::skip_permission_denied, ec);
         it != fs::directory_iterator();
         it.increment(ec)) {
        if (ec) {
            ec.clear();
            continue;
        }
        const auto& entry = *it;
        std::string name = entry.path().filename().string();
        // 目录项追加 '/' 后缀，便于区分文件与目录
        if (entry.is_directory(ec)) {
            name += "/";
        }
        entries.push_back(std::move(name));
    }

    // 按字典序排序，保证输出稳定
    std::sort(entries.begin(), entries.end());

    if (entries.empty()) {
        return ToolResult::ok(std::string{"(empty directory)"});
    }

    std::string result;
    result.reserve(entries.size() * 32);
    for (const auto& e : entries) {
        result += e;
        result += '\n';
    }
    if (!result.empty()) {
        result.pop_back();
    }
    return ToolResult::ok(std::move(result));
}

// ============================================================
// 执行
// ============================================================

ToolResult FileReadTool::call(
    const nlohmann::json& input,
    const ToolContext& ctx
) {
    // 1. 解析输入 JSON 为 FileReadInput 结构
    FileReadInput read_input = input.get<FileReadInput>();

    // 读取可配置参数（回退到 constants.h 编译期默认值）
    // prompt() 仍使用 constants.h 作为文档默认值，实际限制由此处配置决定
    const size_t max_file_size = static_cast<size_t>(
        agent::ConfigManager::instance().get_or<int>(
            agent::keys::FILE_READ_MAX_SIZE,
            static_cast<int>(constants::MAX_FILE_SIZE_BYTES)
        )
    );
    const int max_lines = agent::ConfigManager::instance().get_or<int>(
        agent::keys::FILE_READ_MAX_LINES,
        constants::MAX_LINES_TO_READ
    );

    // 2. 路径解析：相对路径基于 ctx.cwd 解析，再规范化为绝对路径
    fs::path file_path(read_input.file_path);
    if (file_path.is_relative()) {
        if (!ctx.cwd.empty()) {
            file_path = fs::path(ctx.cwd) / file_path;
        }
    }
    std::error_code ec;
    file_path = fs::weakly_canonical(file_path, ec);
    if (ec) {
        ec.clear();  // 规范化失败不中断，继续使用原路径
    }

    // 3. 检查路径存在性
    if (!fs::exists(file_path, ec)) {
        return ToolResult::error("File does not exist: " + read_input.file_path);
    }

    // 4. 目录处理：路径指向目录时列举内容
    if (fs::is_directory(file_path, ec)) {
        return read_directory(file_path);
    }

    // 5. 非常规文件（如设备文件、管道）拒绝读取
    if (!fs::is_regular_file(file_path, ec)) {
        return ToolResult::error("Path is not a regular file: " + read_input.file_path);
    }

    // 6. 文件大小检查：超过 max_file_size 拒绝（需用 offset/limit 分段）
    const auto file_size = fs::file_size(file_path, ec);
    if (!ec && file_size > max_file_size) {
        return ToolResult::error(std::format(
            "File size {} bytes exceeds maximum {} bytes; use offset and limit for larger files",
            file_size, max_file_size
        ));
    }

    // 7. 编码检测（替代原 is_binary_file，更精确：UTF-16 不会被误判为二进制）
    const Encoding encoding = detect_encoding(file_path);
    if (encoding == Encoding::Binary) {
        return ToolResult::error("File appears to be binary, cannot display: " + read_input.file_path);
    }

    const int offset = read_input.offset.value_or(1);
    const int limit = read_input.limit.value_or(max_lines);

    std::vector<std::string> target_lines;
    int total_lines = 0;
    bool has_more = false;

    if (encoding == Encoding::Utf8 || encoding == Encoding::Ascii
        || encoding == Encoding::Unknown) {
        // 8a. UTF-8/ASCII：流式读取（仅存储目标范围行，避免全部加载到内存）
        std::ifstream file(file_path);
        if (!file.is_open()) {
            return ToolResult::error("Failed to open file: " + read_input.file_path);
        }

        // 跳过 UTF-8 BOM（若存在）
        char bom[3];
        file.read(bom, 3);
        if (file.gcount() == 3
            && static_cast<unsigned char>(bom[0]) == 0xEF
            && static_cast<unsigned char>(bom[1]) == 0xBB
            && static_cast<unsigned char>(bom[2]) == 0xBF) {
            // BOM 已跳过
        } else {
            file.seekg(0); // 无 BOM，回到开头
        }

        target_lines.reserve(static_cast<size_t>(std::min(limit, max_lines)));
        std::string line;

        // 单次遍历：计数所有行，仅存储 [offset, offset+limit) 范围内的行
        while (std::getline(file, line)) {
            ++total_lines;
            if (total_lines >= offset && total_lines < offset + limit) {
                normalize_eol(line);  // 剥离 CRLF 残留的 '\r'
                target_lines.push_back(std::move(line));
            }
            // 已读取到目标范围末尾，检查是否还有更多行后提前退出
            if (total_lines >= offset + limit) {
                std::string extra;
                if (std::getline(file, extra)) {
                    has_more = true;
                }
                break;
            }
        }
    } else {
        // 8b. 非 UTF-8（UTF-16/GBK）：全量读取 + 编码转换为 UTF-8
        std::vector<std::string> all_lines = read_as_utf8_lines(file_path, encoding);
        total_lines = static_cast<int>(all_lines.size());

        // 应用 offset/limit 切片
        if (offset <= total_lines) {
            const int end = std::min(offset - 1 + limit, total_lines);
            target_lines.assign(
                all_lines.begin() + (offset - 1),
                all_lines.begin() + end
            );
        }
    }

    // 空文件检查：返回提示信息而非空字符串（对齐 Claude Code 行为）
    if (total_lines == 0) {
        return ToolResult::ok(std::string{"<system-reminder>File exists but has empty contents.</system-reminder>"});
    }

    // offset 超出范围
    if (offset > total_lines) {
        return ToolResult::error(std::format(
            "offset {} is beyond the file's {} lines", offset, total_lines
        ));
    }

    // 9. 格式化输出（带行号，1-based）
    std::string formatted = format_with_line_numbers(target_lines, offset);

    // 10. 部分读取时附加元信息（行数统计）
    const int lines_read = static_cast<int>(target_lines.size());
    if (has_more) {
        // 提前退出：文件还有更多行，total_lines 不完整
        formatted += "\n\n(" + std::to_string(lines_read) + " lines shown, more available)";
    } else if (lines_read < total_lines) {
        formatted += "\n\n(" + std::to_string(lines_read) + " of "
                   + std::to_string(total_lines) + " lines shown)";
    }

    return ToolResult::ok(std::move(formatted));
}

} // namespace agent::tool
