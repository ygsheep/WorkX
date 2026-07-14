/**
 * @file encoding.cpp
 * @brief 文件编码检测与转换实现
 * @details 实现 BOM 检测、UTF-8 验证、GBK 启发式检测，
 *          以及 UTF-16→UTF-8 和 GBK→UTF-8 编码转换。
 * @author workx
 * @version 1.0.0
 * @date 2026-07
 */

#include "agent/tool/encoding.h"

#include <fstream>
#include <sstream>
#include <cstdint>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#endif

namespace agent::tool {

namespace fs = std::filesystem;

// ============================================================
// 内部辅助函数
// ============================================================

/// @brief UTF-16 LE → UTF-8 转换
static std::string utf16le_to_utf8(const char* data, size_t size) {
    std::string result;
    result.reserve(size);

    for (size_t i = 0; i + 1 < size; i += 2) {
        const uint16_t cu = static_cast<uint8_t>(data[i])
                          | (static_cast<uint8_t>(data[i + 1]) << 8);

        // 高代理项（surrogate pair）
        if (cu >= 0xD800 && cu <= 0xDBFF) {
            if (i + 3 < size) {
                const uint16_t lo = static_cast<uint8_t>(data[i + 2])
                                  | (static_cast<uint8_t>(data[i + 3]) << 8);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    const uint32_t cp = 0x10000 + ((cu - 0xD800) << 10) + (lo - 0xDC00);
                    result += static_cast<char>(0xF0 | (cp >> 18));
                    result += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                    result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    result += static_cast<char>(0x80 | (cp & 0x3F));
                    ++i; // 跳过低代理项
                    continue;
                }
            }
            continue; // 无效高代理项，跳过
        }

        // 孤立低代理项，跳过
        if (cu >= 0xDC00 && cu <= 0xDFFF) continue;

        // BMP 字符
        if (cu < 0x80) {
            result += static_cast<char>(cu);
        } else if (cu < 0x800) {
            result += static_cast<char>(0xC0 | (cu >> 6));
            result += static_cast<char>(0x80 | (cu & 0x3F));
        } else {
            result += static_cast<char>(0xE0 | (cu >> 12));
            result += static_cast<char>(0x80 | ((cu >> 6) & 0x3F));
            result += static_cast<char>(0x80 | (cu & 0x3F));
        }
    }
    return result;
}

/// @brief UTF-16 BE → UTF-8 转换
static std::string utf16be_to_utf8(const char* data, size_t size) {
    std::string result;
    result.reserve(size);

    for (size_t i = 0; i + 1 < size; i += 2) {
        const uint16_t cu = (static_cast<uint8_t>(data[i]) << 8)
                          | static_cast<uint8_t>(data[i + 1]);

        if (cu >= 0xD800 && cu <= 0xDBFF) {
            if (i + 3 < size) {
                const uint16_t lo = (static_cast<uint8_t>(data[i + 2]) << 8)
                                  | static_cast<uint8_t>(data[i + 3]);
                if (lo >= 0xDC00 && lo <= 0xDFFF) {
                    const uint32_t cp = 0x10000 + ((cu - 0xD800) << 10) + (lo - 0xDC00);
                    result += static_cast<char>(0xF0 | (cp >> 18));
                    result += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                    result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                    result += static_cast<char>(0x80 | (cp & 0x3F));
                    ++i;
                    continue;
                }
            }
            continue;
        }

        if (cu >= 0xDC00 && cu <= 0xDFFF) continue;

        if (cu < 0x80) {
            result += static_cast<char>(cu);
        } else if (cu < 0x800) {
            result += static_cast<char>(0xC0 | (cu >> 6));
            result += static_cast<char>(0x80 | (cu & 0x3F));
        } else {
            result += static_cast<char>(0xE0 | (cu >> 12));
            result += static_cast<char>(0x80 | ((cu >> 6) & 0x3F));
            result += static_cast<char>(0x80 | (cu & 0x3F));
        }
    }
    return result;
}

/// @brief GBK → UTF-8 转换（平台相关）
static std::string gbk_to_utf8(const char* data, size_t size) {
#ifdef _WIN32
    // Windows: MultiByteToWideChar (GBK = CP_936)
    const int wlen = MultiByteToWideChar(936, 0, data, static_cast<int>(size), nullptr, 0);
    if (wlen == 0) return std::string(data, size); // 回退

    std::wstring wstr(static_cast<size_t>(wlen), 0);
    MultiByteToWideChar(936, 0, data, static_cast<int>(size), wstr.data(), wlen);

    const int ulen = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), wlen,
                                         nullptr, 0, nullptr, nullptr);
    if (ulen == 0) return std::string(data, size); // 回退

    std::string result(static_cast<size_t>(ulen), 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), wlen,
                        result.data(), ulen, nullptr, nullptr);
    return result;
#else
    // Linux: iconv
    iconv_t cd = iconv_open("UTF-8", "GBK");
    if (cd == reinterpret_cast<iconv_t>(-1)) return std::string(data, size);

    size_t in_left = size;
    size_t out_left = size * 3; // 最坏情况
    std::string result(out_left, 0);

    char* in_buf = const_cast<char*>(data);
    char* out_buf = result.data();

    const size_t ret = iconv(cd, &in_buf, &in_left, &out_buf, &out_left);
    iconv_close(cd);

    if (ret == static_cast<size_t>(-1)) return std::string(data, size); // 回退

    result.resize(result.size() - out_left);
    return result;
#endif
}

/// @brief 验证字节序列是否为合法 UTF-8
/// @return 0=非UTF-8, 1=纯ASCII, 2=含多字节序列的UTF-8
static int validate_utf8(const unsigned char* buf, size_t len) {
    bool has_multibyte = false;
    for (size_t i = 0; i < len; ) {
        const unsigned char c = buf[i];
        if (c < 0x80) {
            ++i;
        } else if (c < 0xC2) {
            return 0; // 非法首字节
        } else if (c < 0xE0) {
            if (i + 1 >= len || (buf[i + 1] & 0xC0) != 0x80) return 0;
            has_multibyte = true;
            i += 2;
        } else if (c < 0xF0) {
            if (i + 2 >= len || (buf[i + 1] & 0xC0) != 0x80 || (buf[i + 2] & 0xC0) != 0x80) return 0;
            has_multibyte = true;
            i += 3;
        } else if (c < 0xF5) {
            if (i + 3 >= len || (buf[i + 1] & 0xC0) != 0x80
                || (buf[i + 2] & 0xC0) != 0x80 || (buf[i + 3] & 0xC0) != 0x80) return 0;
            has_multibyte = true;
            i += 4;
        } else {
            return 0;
        }
    }
    return has_multibyte ? 2 : 1;
}

/// @brief GBK 启发式检测
/// @details GBK 双字节：首字节 0x81-0xFE，次字节 0x40-0xFE（不含 0x7F）
static bool is_likely_gbk(const unsigned char* buf, size_t len) {
    bool has_double = false;
    for (size_t i = 0; i < len; ) {
        const unsigned char c = buf[i];
        if (c < 0x80) {
            ++i;
        } else if (c >= 0x81 && c <= 0xFE) {
            if (i + 1 >= len) break; // 最后一个字节无法判断
            const unsigned char c2 = buf[i + 1];
            if (c2 >= 0x40 && c2 <= 0xFE && c2 != 0x7F) {
                has_double = true;
                i += 2;
            } else {
                return false; // 不符合 GBK 模式
            }
        } else {
            return false;
        }
    }
    return has_double;
}

// ============================================================
// 公共接口实现
// ============================================================

Encoding detect_encoding(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return Encoding::Unknown;

    constexpr size_t CHECK_SIZE = 8192;
    unsigned char buf[CHECK_SIZE];
    file.read(reinterpret_cast<char*>(buf), CHECK_SIZE);
    const size_t n = static_cast<size_t>(file.gcount());

    if (n == 0) return Encoding::Utf8; // 空文件视为 UTF-8

    // 1. BOM 检测
    if (n >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF)
        return Encoding::Utf8;
    if (n >= 2 && buf[0] == 0xFF && buf[1] == 0xFE)
        return Encoding::Utf16LE;
    if (n >= 2 && buf[0] == 0xFE && buf[1] == 0xFF)
        return Encoding::Utf16BE;

    // 2. null 字节检测（二进制判定，UTF-16 已由 BOM 排除）
    for (size_t i = 0; i < n; ++i) {
        if (buf[i] == 0) return Encoding::Binary;
    }

    // 3. UTF-8 验证
    const int utf8_type = validate_utf8(buf, n);
    if (utf8_type == 1) return Encoding::Ascii;
    if (utf8_type == 2) return Encoding::Utf8;

    // 4. GBK 启发式检测
    if (is_likely_gbk(buf, n)) return Encoding::Gbk;

    return Encoding::Unknown;
}

std::vector<std::string> read_as_utf8_lines(const fs::path& path, Encoding encoding) {
    std::vector<std::string> lines;

    // UTF-8/ASCII：直接读取（跳过 BOM）
    if (encoding == Encoding::Utf8 || encoding == Encoding::Ascii
        || encoding == Encoding::Unknown) {
        std::ifstream file(path);
        if (!file.is_open()) return lines;

        // 检测并跳过 UTF-8 BOM
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

        std::string line;
        while (std::getline(file, line)) {
            lines.push_back(std::move(line));
        }
        return lines;
    }

    // 非 UTF-8：全量读取 + 编码转换
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) return lines;

    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());
    file.close();

    std::string utf8_content;
    size_t skip = 0; // 跳过 BOM

    switch (encoding) {
        case Encoding::Utf16LE:
            skip = 2; // FF FE
            utf8_content = utf16le_to_utf8(content.data() + skip, content.size() - skip);
            break;
        case Encoding::Utf16BE:
            skip = 2; // FE FF
            utf8_content = utf16be_to_utf8(content.data() + skip, content.size() - skip);
            break;
        case Encoding::Gbk:
            utf8_content = gbk_to_utf8(content.data(), content.size());
            break;
        default:
            utf8_content = content;
            break;
    }

    // 按行分割
    std::istringstream stream(utf8_content);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(std::move(line));
    }
    return lines;
}

const char* encoding_name(Encoding encoding) {
    switch (encoding) {
        case Encoding::Utf8:    return "UTF-8";
        case Encoding::Utf16LE: return "UTF-16LE";
        case Encoding::Utf16BE: return "UTF-16BE";
        case Encoding::Gbk:     return "GBK";
        case Encoding::Ascii:   return "ASCII";
        case Encoding::Binary:  return "Binary";
        default:                return "Unknown";
    }
}

} // namespace agent::tool
