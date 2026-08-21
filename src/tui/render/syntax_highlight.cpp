#include "render/syntax_highlight.h"

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace ftxtui {

using ftxui::Color;
using ftxui::Element;
using ftxui::Elements;

namespace {

// ---- 配色（One Dark 风格）----
const Color kKeyword = Color::BlueLight;    // 关键字
const Color kType = Color::CyanLight;       // 内置类型/库标识
const Color kString = Color::GreenLight;    // 字符串
const Color kComment = Color::GrayDark;     // 注释
const Color kNumber = Color::YellowLight;   // 数字
const Color kPreproc = Color::MagentaLight; // 预处理指令

// ---- 关键字表（常见语言子集）----

const std::string_view kCppKeywords[] = {
    "alignas", "alignof", "and", "asm", "auto", "bool", "break", "case",
    "catch", "char", "class", "const", "constexpr", "const_cast", "continue",
    "decltype", "default", "delete", "do", "double", "dynamic_cast", "else",
    "enum", "explicit", "export", "extern", "false", "float", "for", "friend",
    "goto", "if", "inline", "int", "long", "mutable", "namespace", "new",
    "noexcept", "nullptr", "operator", "private", "protected", "public",
    "register", "reinterpret_cast", "return", "short", "signed", "sizeof",
    "static", "static_cast", "struct", "switch", "template", "this", "throw",
    "true", "try", "typedef", "typeid", "typename", "union", "unsigned",
    "using", "virtual", "void", "volatile", "while",
};
const std::string_view kCppTypes[] = {
    "std", "string", "vector", "map", "set", "shared_ptr", "unique_ptr",
    "optional", "size_t", "int8_t", "int16_t", "int32_t", "int64_t",
    "uint8_t", "uint16_t", "uint32_t", "uint64_t", "FILE", "ostream",
    "istream", "ifstream", "ofstream", "string_view", "function", "variant",
};

const std::string_view kPythonKeywords[] = {
    "and", "as", "assert", "async", "await", "break", "class", "continue",
    "def", "del", "elif", "else", "except", "False", "finally", "for", "from",
    "global", "if", "import", "in", "is", "lambda", "None", "nonlocal", "not",
    "or", "pass", "raise", "return", "True", "try", "while", "with", "yield",
};
const std::string_view kPythonTypes[] = {
    "print", "len", "range", "int", "float", "str", "bool", "list", "dict",
    "set", "tuple", "self", "super", "open", "enumerate", "zip", "map",
    "filter", "sorted", "type", "object",
};

const std::string_view kJsKeywords[] = {
    "async", "await", "break", "case", "catch", "class", "const", "continue",
    "debugger", "default", "delete", "do", "else", "export", "extends",
    "false", "finally", "for", "from", "function", "get", "if", "import", "in",
    "instanceof", "let", "new", "null", "of", "return", "set", "static",
    "super", "switch", "this", "throw", "true", "try", "typeof", "undefined",
    "var", "void", "while", "yield",
};
const std::string_view kJsTypes[] = {
    "console", "Object", "Array", "String", "Number", "Boolean", "Promise",
    "Date", "JSON", "Math", "Symbol", "Map", "Set", "document", "window",
    "process", "require", "exports", "module", "Buffer",
};

const std::string_view kRustKeywords[] = {
    "as", "async", "await", "break", "const", "continue", "crate", "dyn",
    "else", "enum", "extern", "false", "fn", "for", "if", "impl", "in", "let",
    "loop", "match", "mod", "move", "mut", "pub", "ref", "return", "self",
    "Self", "static", "struct", "super", "trait", "true", "type", "unsafe",
    "use", "where", "while",
};
const std::string_view kRustTypes[] = {
    "Option", "Result", "String", "Vec", "Box", "Some", "None", "Ok", "Err",
    "HashMap", "i32", "i64", "u32", "u64", "usize", "f32", "f64", "bool",
    "char", "str", "println", "println!", "format", "panic",
};

const std::string_view kGoKeywords[] = {
    "break", "case", "chan", "const", "continue", "default", "defer", "else",
    "fallthrough", "for", "func", "go", "goto", "if", "import", "interface",
    "map", "package", "range", "return", "select", "struct", "switch", "type",
    "var",
};
const std::string_view kGoTypes[] = {
    "bool", "byte", "error", "float32", "float64", "int", "int8", "int16",
    "int32", "int64", "rune", "string", "uint", "uint8", "uint16", "uint32",
    "uint64", "nil", "true", "false", "make", "len", "cap", "append", "fmt",
    "Println", "Printf", "New",
};

const std::string_view kBashKeywords[] = {
    "if", "then", "else", "elif", "fi", "for", "while", "until", "do", "done",
    "case", "esac", "function", "in", "local", "export", "return", "break",
    "continue", "echo", "cd", "exit", "source", "read", "set", "shift",
    "trap", "unset", "alias",
};
const std::string_view kBashTypes[] = {
    "$@", "$?", "$$", "$#", "$0", "$1", "true", "false", "[[", "]]", "&&",
    "||", "2>&1", "sudo", "grep", "sed", "awk", "cat", "ls", "rm", "cp",
    "mv", "mkdir", "touch", "find", "git", "pip", "npm", "python",
};

const std::string_view kSqlKeywords[] = {
    "SELECT", "FROM", "WHERE", "INSERT", "INTO", "VALUES", "UPDATE", "SET",
    "DELETE", "CREATE", "TABLE", "DROP", "ALTER", "ADD", "COLUMN", "INDEX",
    "VIEW", "JOIN", "INNER", "LEFT", "RIGHT", "FULL", "OUTER", "ON", "AS",
    "AND", "OR", "NOT", "NULL", "IS", "IN", "LIKE", "BETWEEN", "ORDER",
    "GROUP", "BY", "HAVING", "LIMIT", "OFFSET", "DISTINCT", "COUNT", "SUM",
    "AVG", "MIN", "MAX", "PRIMARY", "KEY", "FOREIGN", "REFERENCES", "CASE",
    "WHEN", "THEN", "ELSE", "END", "UNION", "ALL", "EXISTS", "DEFAULT",
    "UNIQUE", "CHECK", "CONSTRAINT", "BEGIN", "COMMIT", "ROLLBACK", "TRANSACTION",
};
const std::string_view kSqlTypes[] = {
    "INT", "INTEGER", "BIGINT", "TEXT", "VARCHAR", "CHAR", "BOOLEAN", "DATE",
    "TIMESTAMP", "REAL", "DOUBLE", "NUMERIC", "DECIMAL", "BLOB", "DATETIME",
};

const std::string_view kJsonKeywords[] = {"true", "false", "null"};
const std::string_view kYamlKeywords[] = {"true", "false", "null", "yes", "no", "on", "off"};

struct LangSpec {
    std::string_view line_comment;  ///< 行注释前缀（空 = 无）
    std::string_view block_begin;   ///< 块注释开始（空 = 无；仅行内识别）
    std::string_view block_end;     ///< 块注释结束
    bool hash_preproc = false;      ///< 行首 # 整行着色（cpp 预处理）
    const std::string_view* keywords = nullptr;
    std::size_t keyword_count = 0;
    const std::string_view* types = nullptr;
    std::size_t type_count = 0;
    bool sql_upper = false;         ///< SQL 关键字大小写不敏感
};

const std::string_view kEmptyView;

const LangSpec& spec_for(std::string_view lang) {
    static const LangSpec none{};
    if (lang.empty()) return none;
    if (lang == "cpp" || lang == "c" || lang == "c++" || lang == "cxx" ||
        lang == "cc" || lang == "h" || lang == "hpp") {
        static const LangSpec s{
            .line_comment = "//", .block_begin = "/*", .block_end = "*/",
            .hash_preproc = true,
            .keywords = kCppKeywords, .keyword_count = sizeof(kCppKeywords) / sizeof(kCppKeywords[0]),
            .types = kCppTypes, .type_count = sizeof(kCppTypes) / sizeof(kCppTypes[0]),
        };
        return s;
    }
    if (lang == "python" || lang == "py") {
        static const LangSpec s{
            .line_comment = "#",
            .keywords = kPythonKeywords, .keyword_count = sizeof(kPythonKeywords) / sizeof(kPythonKeywords[0]),
            .types = kPythonTypes, .type_count = sizeof(kPythonTypes) / sizeof(kPythonTypes[0]),
        };
        return s;
    }
    if (lang == "js" || lang == "javascript" || lang == "jsx" ||
        lang == "ts" || lang == "typescript" || lang == "tsx") {
        static const LangSpec s{
            .line_comment = "//", .block_begin = "/*", .block_end = "*/",
            .keywords = kJsKeywords, .keyword_count = sizeof(kJsKeywords) / sizeof(kJsKeywords[0]),
            .types = kJsTypes, .type_count = sizeof(kJsTypes) / sizeof(kJsTypes[0]),
        };
        return s;
    }
    if (lang == "rust" || lang == "rs") {
        static const LangSpec s{
            .line_comment = "//", .block_begin = "/*", .block_end = "*/",
            .keywords = kRustKeywords, .keyword_count = sizeof(kRustKeywords) / sizeof(kRustKeywords[0]),
            .types = kRustTypes, .type_count = sizeof(kRustTypes) / sizeof(kRustTypes[0]),
        };
        return s;
    }
    if (lang == "go" || lang == "golang") {
        static const LangSpec s{
            .line_comment = "//", .block_begin = "/*", .block_end = "*/",
            .keywords = kGoKeywords, .keyword_count = sizeof(kGoKeywords) / sizeof(kGoKeywords[0]),
            .types = kGoTypes, .type_count = sizeof(kGoTypes) / sizeof(kGoTypes[0]),
        };
        return s;
    }
    if (lang == "bash" || lang == "sh" || lang == "shell" || lang == "zsh" ||
        lang == "console" || lang == "shell-session") {
        static const LangSpec s{
            .line_comment = "#",
            .keywords = kBashKeywords, .keyword_count = sizeof(kBashKeywords) / sizeof(kBashKeywords[0]),
            .types = kBashTypes, .type_count = sizeof(kBashTypes) / sizeof(kBashTypes[0]),
        };
        return s;
    }
    if (lang == "sql") {
        static const LangSpec s{
            .line_comment = "--",
            .keywords = kSqlKeywords, .keyword_count = sizeof(kSqlKeywords) / sizeof(kSqlKeywords[0]),
            .types = kSqlTypes, .type_count = sizeof(kSqlTypes) / sizeof(kSqlTypes[0]),
            .sql_upper = true,
        };
        return s;
    }
    if (lang == "json") {
        static const LangSpec s{
            .keywords = kJsonKeywords, .keyword_count = sizeof(kJsonKeywords) / sizeof(kJsonKeywords[0]),
        };
        return s;
    }
    if (lang == "yaml" || lang == "yml") {
        static const LangSpec s{
            .line_comment = "#",
            .keywords = kYamlKeywords, .keyword_count = sizeof(kYamlKeywords) / sizeof(kYamlKeywords[0]),
        };
        return s;
    }
    return none;
}

bool is_ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

bool word_in(const std::string_view& w, const std::string_view* table, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
        if (w == table[i]) return true;
    return false;
}

std::string upper(std::string_view w) {
    std::string s(w);
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

/// @brief 分词并着色一行；返回 false 表示整行原样输出（无任何命中）
Element tokenize_line(std::string_view line, const LangSpec& spec) {
    Elements toks;
    size_t i = 0;
    const size_t n = line.size();
    std::string plain;

    auto flush_plain = [&]() {
        if (!plain.empty()) {
            toks.push_back(ftxui::text(plain));
            plain.clear();
        }
    };

    while (i < n) {
        char c = line[i];

        // 行注释：整行剩余着色后结束
        if (!spec.line_comment.empty() && line.substr(i).starts_with(spec.line_comment)) {
            flush_plain();
            toks.push_back(ftxui::color(kComment)(ftxui::text(std::string(line.substr(i)))));
            return ftxui::hbox(std::move(toks));
        }
        // 行内块注释：/* ... */ 或到行尾
        if (!spec.block_begin.empty() && line.substr(i).starts_with(spec.block_begin)) {
            flush_plain();
            size_t end = line.find(spec.block_end, i + spec.block_begin.size());
            size_t len = (end == std::string_view::npos)
                ? n - i
                : end + spec.block_end.size() - i;
            toks.push_back(ftxui::color(kComment)(ftxui::text(std::string(line.substr(i, len)))));
            i += len;
            continue;
        }
        // 字符串
        if (c == '"' || c == '\'') {
            flush_plain();
            size_t j = i + 1;
            while (j < n) {
                if (line[j] == '\\') { j += 2; continue; }
                if (line[j] == c) { ++j; break; }
                ++j;
            }
            toks.push_back(ftxui::color(kString)(ftxui::text(std::string(line.substr(i, j - i)))));
            i = j;
            continue;
        }
        // 数字
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && i + 1 < n &&
             std::isdigit(static_cast<unsigned char>(line[i + 1])))) {
            flush_plain();
            size_t j = i;
            while (j < n && (std::isalnum(static_cast<unsigned char>(line[j])) ||
                             line[j] == '.' || line[j] == '_' || line[j] == '-' ||
                             line[j] == '+')) {
                // 数字后的 '-'/'+' 多属运算符；仅在紧邻指数 e/E 时算数
                if ((line[j] == '-' || line[j] == '+') &&
                    (j == i || (line[j - 1] != 'e' && line[j - 1] != 'E')))
                    break;
                ++j;
            }
            toks.push_back(ftxui::color(kNumber)(ftxui::text(std::string(line.substr(i, j - i)))));
            i = j;
            continue;
        }
        // 标识符
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t j = i;
            while (j < n && is_ident_char(line[j])) ++j;
            std::string_view w = line.substr(i, j - i);
            Color color = Color::Default;
            if (spec.sql_upper) {
                std::string uw = upper(w);
                if (word_in(uw, spec.keywords, spec.keyword_count)) color = kKeyword;
                else if (spec.types && word_in(uw, spec.types, spec.type_count)) color = kType;
            } else {
                if (word_in(w, spec.keywords, spec.keyword_count)) color = kKeyword;
                else if (spec.types && word_in(w, spec.types, spec.type_count)) color = kType;
            }
            flush_plain();
            auto e = ftxui::text(std::string(w));
            if (color != Color::Default) e = ftxui::color(color)(e);
            toks.push_back(e);
            i = j;
            continue;
        }
        plain.push_back(c);
        ++i;
    }
    flush_plain();
    return ftxui::hbox(std::move(toks));
}

}  // namespace

Element highlight_code_line(std::string_view line, std::string_view lang) {
    if (line.empty()) return ftxui::text("");
    const LangSpec& spec = spec_for(lang);
    if (spec.keywords == nullptr && spec.line_comment.empty() && spec.block_begin.empty()) {
        // 未支持的语言：原样输出（与旧行为一致）
        return ftxui::text(std::string(line));
    }
    // cpp 行首 #：整行预处理着色
    if (spec.hash_preproc && !line.empty() && line.front() == '#') {
        return ftxui::color(kPreproc)(ftxui::text(std::string(line)));
    }
    return tokenize_line(line, spec);
}

}  // namespace ftxtui
