/**
 * @file syntax_highlighter.cpp
 * @brief Tree-sitter 语法高亮实现
 * @details
 *   设计要点:
 *   1. grammar 注册表: 进程级单例, 首次调用时按 WORKX_TS_<NAME> 宏探测可用 grammar
 *   2. parser 池: 每种语言复用一个 TSParser, 避免 ts_parser_new 开销
 *   3. 着色规则: 按 tree-sitter 节点 type 字符串映射到 SyntaxColor
 *      - 关键字: type 本身就是关键字文本 (tree-sitter 命名约定, 如 "if"/"for"/"return")
 *      - 字符串/注释/数字: type 含 string/comment/number 子串
 *      - 类型: primitive_type / type_identifier
 *      - 函数: identifier 在 call_expression / function_declarator 上下文 (查 parent)
 *   4. 输出保证: 每行自包含 ANSI, 行末 RESET, 跨 \n 重新发色
 *   5. 降级: 未启用 / 未知 lang / parse 失败 → 原样返回
 */

#include "tui/render/syntax_highlighter.h"
#include "tui/utils/utf8_utils.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef WORKX_HAS_TREE_SITTER
#include <tree_sitter/api.h>

// 各 grammar 的入口函数 - 仅在对应 WORKX_TS_<NAME> 定义时声明
// 避免 linker 在该 grammar 未安装时报 unresolved symbol
extern "C" {
#ifdef WORKX_TS_C
    const TSLanguage* tree_sitter_c(void);
#endif
#ifdef WORKX_TS_CPP
    const TSLanguage* tree_sitter_cpp(void);
#endif
#ifdef WORKX_TS_CMAKE
    const TSLanguage* tree_sitter_cmake(void);
#endif
#ifdef WORKX_TS_PYTHON
    const TSLanguage* tree_sitter_python(void);
#endif
#ifdef WORKX_TS_BASH
    const TSLanguage* tree_sitter_bash(void);
#endif
#ifdef WORKX_TS_JSON
    const TSLanguage* tree_sitter_json(void);
#endif
#ifdef WORKX_TS_JAVASCRIPT
    const TSLanguage* tree_sitter_javascript(void);
#endif
#ifdef WORKX_TS_RUST
    const TSLanguage* tree_sitter_rust(void);
#endif
#ifdef WORKX_TS_GO
    const TSLanguage* tree_sitter_go(void);
#endif
}
#endif // WORKX_HAS_TREE_SITTER

namespace agent {

// ============================================================================
// 公共工具: strip_ansi / ansi_display_width
// ============================================================================

std::string strip_ansi(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        if (text[i] == '\x1b' && i + 1 < text.size() && text[i + 1] == '[') {
            // CSI 序列: ESC [ <params> <final byte in 0x40-0x7e>
            i += 2;
            while (i < text.size()) {
                char c = text[i];
                ++i;
                if (c >= 0x40 && c <= 0x7e) break;  // final byte
            }
        } else {
            out.push_back(text[i]);
            ++i;
        }
    }
    return out;
}

int ansi_display_width(std::string_view text) {
    return display_width(strip_ansi(text));
}

// ============================================================================
// 未启用 Tree-sitter 的降级实现
// ============================================================================

#ifndef WORKX_HAS_TREE_SITTER

bool syntax_highlighting_enabled() { return false; }

std::string highlight_code(std::string_view /*lang*/, std::string_view code) {
    return std::string(code);
}

#else // WORKX_HAS_TREE_SITTER

bool syntax_highlighting_enabled() { return true; }

// ============================================================================
// Grammar 注册表
// ============================================================================

namespace {

/// 把 markdown 代码块 lang 标签规范化为 grammar key
std::string normalize_lang(std::string_view lang) {
    // trim + lower
    size_t b = 0, e = lang.size();
    while (b < e && (lang[b] == ' ' || lang[b] == '\t')) ++b;
    while (e > b && (lang[e - 1] == ' ' || lang[e - 1] == '\t')) --e;
    std::string s(lang.substr(b, e - b));
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    // 别名归一
    if (s == "cc" || s == "cxx" || s == "hpp" || s == "h" || s == "h++") s = "cpp";
    if (s == "js" || s == "jsx") s = "javascript";
    if (s == "ts" || s == "tsx") s = "typescript";
    if (s == "py" || s == "python3") s = "python";
    if (s == "sh" || s == "shell" || s == "zsh") s = "bash";
    if (s == "gyp") s = "python";
    if (s == "patch" || s == "udiff") s = "diff";
    return s;
}

struct GrammarRegistry {
    std::unordered_map<std::string, const TSLanguage*> langs;
    std::mutex mtx;

    GrammarRegistry() {
        #ifdef WORKX_TS_C
            langs["c"] = tree_sitter_c();
        #endif
        #ifdef WORKX_TS_CPP
            langs["cpp"] = tree_sitter_cpp();
            langs["c++"] = tree_sitter_cpp();
        #endif
        #ifdef WORKX_TS_CMAKE
            langs["cmake"] = tree_sitter_cmake();
        #endif
        #ifdef WORKX_TS_PYTHON
            langs["python"] = tree_sitter_python();
        #endif
        #ifdef WORKX_TS_BASH
            langs["bash"] = tree_sitter_bash();
        #endif
        #ifdef WORKX_TS_JSON
            langs["json"] = tree_sitter_json();
        #endif
        #ifdef WORKX_TS_JAVASCRIPT
            langs["javascript"] = tree_sitter_javascript();
        #endif
        #ifdef WORKX_TS_RUST
            langs["rust"] = tree_sitter_rust();
        #endif
        #ifdef WORKX_TS_GO
            langs["go"] = tree_sitter_go();
        #endif
    }
};

GrammarRegistry& registry() {
    static GrammarRegistry inst;
    return inst;
}

/// parser 池 - 每种语言一个, 复用避免 ts_parser_new 开销
TSParser* get_parser_for_lang(const TSLanguage* lang) {
    static std::mutex pool_mtx;
    static std::unordered_map<const TSLanguage*, TSParser*> pool;
    std::lock_guard<std::mutex> lk(pool_mtx);
    auto it = pool.find(lang);
    if (it != pool.end()) return it->second;
    TSParser* p = ts_parser_new();
    if (!ts_parser_set_language(p, lang)) {
        ts_parser_delete(p);
        return nullptr;
    }
    pool[lang] = p;
    return p;
}

// ============================================================================
// 节点类型 → 颜色映射
// ============================================================================

/// 关键字节点 type 集合 (tree-sitter 把关键字 token 的 type 命名为关键字文本本身)
/// 这里收录跨 C/C++/Python/JS/Rust/Go/Bash 的常见关键字
const std::unordered_map<std::string, SyntaxColor>& keyword_set() {
    static const std::unordered_map<std::string, SyntaxColor> kw = {
        // C/C++
        {"if", SyntaxColor::Keyword}, {"else", SyntaxColor::Keyword},
        {"for", SyntaxColor::Keyword}, {"while", SyntaxColor::Keyword},
        {"do", SyntaxColor::Keyword}, {"switch", SyntaxColor::Keyword},
        {"case", SyntaxColor::Keyword}, {"default", SyntaxColor::Keyword},
        {"break", SyntaxColor::Keyword}, {"continue", SyntaxColor::Keyword},
        {"return", SyntaxColor::Keyword}, {"goto", SyntaxColor::Keyword},
        {"class", SyntaxColor::Keyword}, {"struct", SyntaxColor::Keyword},
        {"union", SyntaxColor::Keyword}, {"enum", SyntaxColor::Keyword},
        {"namespace", SyntaxColor::Keyword}, {"typedef", SyntaxColor::Keyword},
        {"using", SyntaxColor::Keyword}, {"template", SyntaxColor::Keyword},
        {"typename", SyntaxColor::Keyword}, {"public", SyntaxColor::Keyword},
        {"private", SyntaxColor::Keyword}, {"protected", SyntaxColor::Keyword},
        {"virtual", SyntaxColor::Keyword}, {"override", SyntaxColor::Keyword},
        {"const", SyntaxColor::Keyword}, {"constexpr", SyntaxColor::Keyword},
        {"static", SyntaxColor::Keyword}, {"inline", SyntaxColor::Keyword},
        {"extern", SyntaxColor::Keyword}, {"register", SyntaxColor::Keyword},
        {"volatile", SyntaxColor::Keyword}, {"mutable", SyntaxColor::Keyword},
        {"auto", SyntaxColor::Keyword}, {"decltype", SyntaxColor::Keyword},
        {"new", SyntaxColor::Keyword}, {"delete", SyntaxColor::Keyword},
        {"this", SyntaxColor::Keyword}, {"nullptr", SyntaxColor::Constant},
        {"true", SyntaxColor::Constant}, {"false", SyntaxColor::Constant},
        {"NULL", SyntaxColor::Constant}, {"sizeof", SyntaxColor::Keyword},
        {"alignof", SyntaxColor::Keyword}, {"alignas", SyntaxColor::Keyword},
        {"operator", SyntaxColor::Keyword}, {"explicit", SyntaxColor::Keyword},
        {"friend", SyntaxColor::Keyword}, {"noexcept", SyntaxColor::Keyword},
        {"throw", SyntaxColor::Keyword}, {"try", SyntaxColor::Keyword},
        {"catch", SyntaxColor::Keyword}, {"co_await", SyntaxColor::Keyword},
        {"co_yield", SyntaxColor::Keyword}, {"co_return", SyntaxColor::Keyword},
        {"requires", SyntaxColor::Keyword}, {"concept", SyntaxColor::Keyword},
        // Python
        {"def", SyntaxColor::Keyword}, {"lambda", SyntaxColor::Keyword},
        {"import", SyntaxColor::Keyword}, {"from", SyntaxColor::Keyword},
        {"as", SyntaxColor::Keyword}, {"pass", SyntaxColor::Keyword},
        {"yield", SyntaxColor::Keyword}, {"global", SyntaxColor::Keyword},
        {"nonlocal", SyntaxColor::Keyword}, {"with", SyntaxColor::Keyword},
        {"async", SyntaxColor::Keyword}, {"await", SyntaxColor::Keyword},
        {"in", SyntaxColor::Keyword}, {"is", SyntaxColor::Keyword},
        {"not", SyntaxColor::Keyword}, {"and", SyntaxColor::Keyword},
        {"or", SyntaxColor::Keyword}, {"None", SyntaxColor::Constant},
        {"True", SyntaxColor::Constant}, {"False", SyntaxColor::Constant},
        // Rust
        {"fn", SyntaxColor::Keyword}, {"let", SyntaxColor::Keyword},
        {"mut", SyntaxColor::Keyword}, {"match", SyntaxColor::Keyword},
        {"impl", SyntaxColor::Keyword}, {"trait", SyntaxColor::Keyword},
        {"pub", SyntaxColor::Keyword}, {"mod", SyntaxColor::Keyword},
        {"crate", SyntaxColor::Keyword}, {"self", SyntaxColor::Keyword},
        {"super", SyntaxColor::Keyword}, {"where", SyntaxColor::Keyword},
        {"unsafe", SyntaxColor::Keyword}, {"move", SyntaxColor::Keyword},
        {"ref", SyntaxColor::Keyword}, {"Some", SyntaxColor::Constant},
        {"Ok", SyntaxColor::Constant}, {"Err", SyntaxColor::Constant},
        // Go
        {"func", SyntaxColor::Keyword}, {"var", SyntaxColor::Keyword},
        {"type", SyntaxColor::Keyword}, {"package", SyntaxColor::Keyword},
        {"range", SyntaxColor::Keyword}, {"chan", SyntaxColor::Keyword},
        {"select", SyntaxColor::Keyword}, {"defer", SyntaxColor::Keyword},
        {"interface", SyntaxColor::Keyword}, {"map", SyntaxColor::Keyword},
        {"nil", SyntaxColor::Constant},
        // JS/TS
        {"function", SyntaxColor::Keyword}, {"var", SyntaxColor::Keyword},
        {"let", SyntaxColor::Keyword}, {"const", SyntaxColor::Keyword},
        {"undefined", SyntaxColor::Constant}, {"typeof", SyntaxColor::Keyword},
        {"instanceof", SyntaxColor::Keyword}, {"void", SyntaxColor::Keyword},
        {"extends", SyntaxColor::Keyword}, {"super", SyntaxColor::Keyword},
        {"export", SyntaxColor::Keyword}, {"async", SyntaxColor::Keyword},
        {"of", SyntaxColor::Keyword},
        // Bash
        {"then", SyntaxColor::Keyword}, {"fi", SyntaxColor::Keyword},
        {"esac", SyntaxColor::Keyword}, {"done", SyntaxColor::Keyword},
        {"local", SyntaxColor::Keyword}, {"declare", SyntaxColor::Keyword},
    };
    return kw;
}

/// 把节点 type 字符串映射到颜色 (不考虑 parent 上下文)
SyntaxColor classify_by_type(std::string_view type) {
    // 1. 精确关键字匹配
    const auto& kw = keyword_set();
    auto it = kw.find(std::string(type));
    if (it != kw.end()) return it->second;

    // E.6：改用精确匹配替代子串匹配
    // 原 contains("number") 会匹配 "number_statement" 等不相关节点；
    // 原 contains("string") 会匹配 "string_content" 等内部节点。
    // 精确匹配覆盖各 grammar 的实际节点 type 命名。
    static const std::unordered_set<std::string_view> comment_types = {
        "comment", "line_comment", "block_comment", "documentation_comment",
        "documentation_comment_prefix", "documentation_comment_suffix"
    };
    static const std::unordered_set<std::string_view> string_types = {
        "string", "string_literal", "char_literal", "raw_string",
        "raw_string_literal", "string_content", "string_array"
    };
    static const std::unordered_set<std::string_view> number_types = {
        "number", "number_literal", "integer", "float",
        "integer_literal", "float_literal",
        "decimal_floating_literal", "hex_literal",
        "octal_literal", "binary_literal"
    };
    static const std::unordered_set<std::string_view> type_types = {
        "primitive_type", "type_identifier", "type_specifier",
        "type_argument", "abstract_type", "concrete_type"
    };
    static const std::unordered_set<std::string_view> constant_types = {
        "built_in_constant", "escape_sequence", "escape",
        "char_escape", "escape_sequence_unicode"
    };

    if (comment_types.count(type))      return SyntaxColor::Comment;
    if (string_types.count(type))       return SyntaxColor::String;
    if (constant_types.count(type))     return SyntaxColor::Constant;
    if (number_types.count(type))       return SyntaxColor::Number;
    if (type_types.count(type))         return SyntaxColor::Type;

    return SyntaxColor::Default;
}

/// 对 identifier 类节点, 检查 parent 上下文决定是否为函数名
SyntaxColor classify_identifier(TSNode node) {
    TSNode parent = ts_node_parent(node);
    if (!ts_node_is_null(parent)) {
        const char* pt = ts_node_type(parent);
        std::string_view ptype(pt);
        auto ends_with = [&](const char* s) {
            size_t n = std::strlen(s);
            return ptype.size() >= n && ptype.substr(ptype.size() - n) == s;
        };
        if (ends_with("call_expression") ||
            ends_with("function_declarator") ||
            ends_with("method_definition") ||
            ends_with("function_definition") ||
            ends_with("function_item") ||         // rust
            ends_with("method_declaration") ||    // go
            ends_with("function_declaration")) {
            return SyntaxColor::Function;
        }
        // 字段访问 / 属性
        if (ends_with("field_access") ||
            ends_with("member_expression") ||
            ends_with("attribute_item") ||
            ends_with("field_declaration")) {
            return SyntaxColor::Property;
        }
    }
    return SyntaxColor::Default;
}

// ============================================================================
// AST 遍历 + 着色输出
// ============================================================================

struct Highlighter {
    std::string_view src;     ///< 原始代码
    std::string out;          ///< 输出 (带 ANSI)
    uint32_t cursor_pos = 0;  ///< 已写入到 src 的位置

    /// 写入 [cursor_pos, end) 范围内的未着色原文
    void emit_raw_until(uint32_t end) {
        if (end <= cursor_pos) return;
        out.append(src.data() + cursor_pos, end - cursor_pos);
        cursor_pos = end;
    }

    /// 写入带颜色包裹的 token 文本, 处理跨行: 每行行末 RESET, 行首重新发色
    void emit_colored(uint32_t start, uint32_t end, SyntaxColor color) {
        if (end <= start) return;
        // 先把 [cursor_pos, start) 的原文 (含空白/标点) 写入
        emit_raw_until(start);

        if (color == SyntaxColor::Default) {
            // 默认色直接写原文, 不包裹 ANSI
            out.append(src.data() + start, end - start);
            cursor_pos = end;
            return;
        }

        auto color_seq = syntax_color_ansi(color);
        constexpr std::string_view reset = "\x1b[0m";

        std::string_view seg(src.data() + start, end - start);
        size_t pos = 0;
        bool color_active = false;
        while (pos < seg.size()) {
            size_t nl = seg.find('\n', pos);
            if (nl == std::string_view::npos) {
                // 段内无更多换行
                if (!color_active) { out.append(color_seq); color_active = true; }
                out.append(seg.data() + pos, seg.size() - pos);
                pos = seg.size();
            } else {
                // 写到换行前
                if (!color_active) { out.append(color_seq); color_active = true; }
                out.append(seg.data() + pos, nl - pos);
                out.append(reset);
                out.push_back('\n');
                color_active = false;
                pos = nl + 1;
            }
        }
        if (color_active) out.append(reset);
        cursor_pos = end;
    }

    /// DFS 遍历语法树
    void walk(TSNode node) {
        uint32_t child_count = ts_node_child_count(node);
        if (child_count == 0) {
            // 叶子节点: 按类型上色
            uint32_t start = ts_node_start_byte(node);
            uint32_t end = ts_node_end_byte(node);
            std::string_view type(ts_node_type(node));

            SyntaxColor c = classify_by_type(type);
            if (c == SyntaxColor::Default && type == "identifier") {
                c = classify_identifier(node);
            }
            emit_colored(start, end, c);
            return;
        }
        // 非叶子: 递归遍历子节点, 节点间未覆盖的原文 (如标点/空白) 由 emit_raw_until 补齐
        for (uint32_t i = 0; i < child_count; ++i) {
            walk(ts_node_child(node, i));
        }
    }

    void finalize() {
        // 末尾未覆盖的原文 (如尾部空白) 写入
        emit_raw_until(static_cast<uint32_t>(src.size()));
    }
};

} // namespace

// ============================================================================
// highlight_code 公共入口
// ============================================================================

/// diff 专用高亮: 不走 tree-sitter, 手工按行处理
/// 规则:
///   - 文件头行 (--- / +++) 和 hunk 头行 (@@ ... @@) 完全不输出
///   - 每行前缀符号 (+/-/space) 不输出, 只输出代码内容
///   - + 行用绿色背景 (DiffAdd), 文本保持默认色
///   - - 行用红色背景 (DiffDelete), 文本保持默认色
///   - 上下文行默认色
std::string highlight_diff(std::string_view file_lang, std::string_view diff) {
    if (diff.empty()) return {};

    // 背景色: 深绿底 / 深红底 (256色, 接近 Claude Code 风格, 低饱和度)
    constexpr auto BG_GREEN = "\x1b[48;5;22m";
    constexpr auto BG_RED   = "\x1b[48;5;52m";
    constexpr auto RESET    = "\x1b[0m";

    // ---- 第 1 步: 解析 diff, 收集代码行 + 行类型 ----
    // E.5：prefix 改用 enum class，避免 char 类型与 -1 比较时在 UTF-8 字节 0xFF 误判
    // （char 在某些平台是 unsigned，-1 会变为 255；在 signed 平台 0xFF 会等于 -1）
    enum class DiffPrefix : int {
        None    = 0,   // 无前缀（容错行）
        Add     = 1,   // '+'
        Del     = 2,   // '-'
        Context = 3,   // ' '
        Header  = 4,   // 文件头 / hunk 头，完全跳过
    };
    struct DiffLine {
        DiffPrefix prefix;
        std::string content;  // 去掉前缀后的内容
    };
    std::vector<DiffLine> lines;
    {
        size_t pos = 0;
        while (pos < diff.size()) {
            size_t nl = diff.find('\n', pos);
            std::string_view line = (nl == std::string_view::npos)
                ? diff.substr(pos)
                : diff.substr(pos, nl - pos);

            DiffLine dl{DiffPrefix::None, {}};
            if (!line.empty()) {
                // 文件头 / hunk 头: 跳过
                bool is_header = false;
                if (line.size() >= 3 && (line.substr(0, 3) == "---" || line.substr(0, 3) == "+++")) {
                    is_header = true;
                } else if (line.size() >= 2 && line.substr(0, 2) == "@@") {
                    is_header = true;
                }
                if (!is_header) {
                    if (line[0] == '+') {
                        dl.prefix = DiffPrefix::Add;
                        dl.content = std::string(line.substr(1));
                    } else if (line[0] == '-') {
                        dl.prefix = DiffPrefix::Del;
                        dl.content = std::string(line.substr(1));
                    } else if (line[0] == ' ') {
                        dl.prefix = DiffPrefix::Context;
                        dl.content = std::string(line.substr(1));
                    } else {
                        // 无前缀行 (理论 diff 不应有, 容错)
                        dl.prefix = DiffPrefix::None;
                        dl.content = std::string(line);
                    }
                } else {
                    // header 不入 lines (完全跳过)
                    dl.prefix = DiffPrefix::Header;
                }
            }
            if (dl.prefix != DiffPrefix::Header && dl.prefix != DiffPrefix::None) {
                lines.push_back(std::move(dl));
            } else if (dl.prefix == DiffPrefix::None && !line.empty()) {
                // 无前缀的非空行也加入 (作为上下文处理)
                lines.push_back(std::move(dl));
            }
            // 空行: 加入 (作为上下文空行)
            if (line.empty()) {
                lines.push_back({DiffPrefix::None, {}});
            }

            if (nl == std::string_view::npos) break;
            pos = nl + 1;
        }
    }

    // ---- 第 2 步: 把所有代码行拼成纯代码块, 送 tree-sitter 前景色高亮 ----
    std::string code_blob;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) code_blob.push_back('\n');
        code_blob += lines[i].content;
    }

    // 用文件语言对整块代码做前景色高亮 (返回带 ANSI 的字符串)
    // 若 file_lang 未知/未启用, hl_lines 就是原始内容 (无 ANSI)
    std::string highlighted = highlight_code(file_lang, code_blob);

    // 拆行
    std::vector<std::string> hl_lines;
    {
        std::string cur;
        for (char c : highlighted) {
            if (c == '\n') { hl_lines.push_back(std::move(cur)); cur.clear(); }
            else cur.push_back(c);
        }
        if (!cur.empty()) hl_lines.push_back(std::move(cur));
        // 行数不一致 (不应发生), 退回未高亮
        if (hl_lines.size() != lines.size()) {
            hl_lines.clear();
            for (const auto& l : lines) hl_lines.push_back(l.content);
        }
    }

    // ---- 第 3 步: 按原 diff 行类型, 给每行加背景色 ----
    // 关键: 前景色 (tree-sitter 已加) + 背景色 (我们加) 组合
    // 行内 tree-sitter 已经在每个 \n 前发 RESET, 会把背景色也 reset 掉
    // 解决: 在每行的每个 RESET 前先补背景色, 让背景延续到行末
    std::string out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) out.push_back('\n');

        const auto prefix = lines[i].prefix;
        const std::string& hl = hl_lines[i];

        // 选背景色
        const char* bg = nullptr;
        if (prefix == DiffPrefix::Add) bg = BG_GREEN;
        else if (prefix == DiffPrefix::Del) bg = BG_RED;
        // 其他 (上下文 / 无前缀): 无背景色

        if (bg == nullptr) {
            // 上下文行: 直接用 tree-sitter 高亮结果
            out.append(hl);
        } else {
            // 有背景色: 在行首发背景色, 然后把内容里的每个 RESET 替换为 "RESET + bg"
            // 这样背景色跨 token 边界保持, 行末最后再 RESET
            out.append(bg);
            size_t sp = 0;
            while (sp < hl.size()) {
                size_t rp = hl.find(RESET, sp);
                if (rp == std::string::npos) {
                    out.append(hl.data() + sp, hl.size() - sp);
                    break;
                }
                // 写到 RESET 之前
                out.append(hl.data() + sp, rp - sp);
                // 写 RESET, 然后立即重新发背景色 (除非是行末最后一次)
                out.append(RESET);
                out.append(bg);
                sp = rp + std::strlen(RESET);
            }
            // 行末 RESET (关掉背景色)
            out.append(RESET);
        }
    }
    return out;
}

std::string highlight_code(std::string_view lang, std::string_view code) {
    if (code.empty()) return {};

    std::string nlang = normalize_lang(lang);

    // diff 走专用处理 (无文件语言信息, 仅画背景色)
    if (nlang == "diff") {
        return highlight_diff("", code);
    }

    if (nlang.empty()) return std::string(code);

    GrammarRegistry& reg = registry();
    std::lock_guard<std::mutex> lk(reg.mtx);
    auto it = reg.langs.find(nlang);
    if (it == reg.langs.end() || it->second == nullptr) {
        return std::string(code);
    }

    TSParser* parser = get_parser_for_lang(it->second);
    if (!parser) return std::string(code);

    // C.13：原代码 `std::string buf(code)` 是冗余拷贝
    // tree-sitter API 接受 const char* + uint32_t length，直接用 string_view 即可
    TSTree* tree = ts_parser_parse_string(parser, nullptr,
                                          code.data(),
                                          static_cast<uint32_t>(code.size()));
    if (!tree) return std::string(code);

    Highlighter h;
    h.src = code;
    h.walk(ts_tree_root_node(tree));
    h.finalize();

    ts_tree_delete(tree);
    return h.out;
}

#endif // WORKX_HAS_TREE_SITTER

} // namespace agent
