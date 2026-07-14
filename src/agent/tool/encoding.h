/**
 * @file encoding.h
 * @brief 文件编码检测与转换
 * @details 支持 UTF-8/UTF-16/GBK/ASCII 编码检测，并将非 UTF-8 内容转换为 UTF-8。
 *          检测顺序：BOM → null 字节（二进制）→ UTF-8 验证 → GBK 启发式。
 * @author workx
 * @version 1.0.0
 * @date 2026-07
 */

#pragma once

#include <string>
#include <vector>
#include <filesystem>

namespace agent::tool {

/// @brief 文件编码类型
enum class Encoding {
    Utf8,       ///< UTF-8（含或不含 BOM）
    Utf16LE,    ///< UTF-16 Little Endian（含 BOM）
    Utf16BE,    ///< UTF-16 Big Endian（含 BOM）
    Gbk,        ///< GBK/GB2312（中文，无 BOM）
    Ascii,      ///< ASCII（纯 7-bit，UTF-8 子集）
    Binary,     ///< 二进制文件（含 null 字节，非 UTF-16）
    Unknown,    ///< 未知编码
};

/// @brief 检测文件编码
/// @details 检测顺序：
///          1. BOM 检测（UTF-8/UTF-16 LE/BE）
///          2. null 字节检测（二进制判定，UTF-16 已由 BOM 排除）
///          3. UTF-8 多字节序列验证
///          4. GBK 双字节启发式检测
/// @param path 文件路径
/// @return 检测到的编码类型；空文件返回 Utf8
Encoding detect_encoding(const std::filesystem::path& path);

/// @brief 读取文件并转换为 UTF-8 行列表
/// @details 根据编码类型选择读取策略：
///          - UTF-8/ASCII：直接读取（跳过 BOM）
///          - UTF-16：全量读取 + 手动转换为 UTF-8
///          - GBK：全量读取 + 平台 API 转换（Windows: MultiByteToWideChar）
/// @param path 文件路径
/// @param encoding 编码类型（通常由 detect_encoding 获取）
/// @return UTF-8 文本行列表；读取失败返回空列表
std::vector<std::string> read_as_utf8_lines(
    const std::filesystem::path& path,
    Encoding encoding
);

/// @brief 获取编码名称（用于日志/错误信息）
/// @param encoding 编码类型
/// @return 编码名称字符串（如 "UTF-8"、"GBK"）
const char* encoding_name(Encoding encoding);

} // namespace agent::tool
