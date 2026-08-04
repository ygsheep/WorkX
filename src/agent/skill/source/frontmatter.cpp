/**
 * @file frontmatter.cpp
 * @brief Skill 文件 frontmatter 解析实现
 * @details 极简 key: value 解析器：按行解析，不支持嵌套 YAML。
 *          当前字段均为扁平标量，够用即可；未来需要嵌套结构再引入 yaml-cpp。
 * @version 1.0.0
 * @date 2026-08
 */

#include "agent/skill/inclaude/frontmatter.h"

#include <cctype>

namespace agent::skill {

namespace {

/// @brief 去首尾空白（含 \r，兼容 CRLF 文件）
std::string trim(std::string_view s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return std::string(s.substr(start, end - start));
}

/// @brief 是否为 `---` 分隔行
bool is_fence(std::string_view line) {
    return trim(line) == "---";
}

/// @brief 解析布尔值；无法识别时返回 fallback
bool parse_bool(std::string_view value, bool fallback) {
    const auto v = trim(value);
    if (v == "true" || v == "yes" || v == "1") return true;
    if (v == "false" || v == "no" || v == "0") return false;
    return fallback;
}

/// @brief 解析 aliases：支持 `a, b` 与 `[a, b]` 形式
std::vector<std::string> parse_aliases(std::string_view value) {
    auto v = trim(value);
    if (v.size() >= 2 && v.front() == '[' && v.back() == ']') {
        v = v.substr(1, v.size() - 2);
    }
    std::vector<std::string> aliases;
    size_t pos = 0;
    while (pos <= v.size()) {
        const size_t comma = v.find(',', pos);
        const auto item = trim(v.substr(pos, comma == std::string_view::npos ? v.size() - pos : comma - pos));
        if (!item.empty()) aliases.push_back(item);
        if (comma == std::string_view::npos) break;
        pos = comma + 1;
    }
    return aliases;
}

/// @brief 从正文首行派生描述（去掉 # 前缀与空白）
std::string derive_description(const std::string& body) {
    const size_t first_newline = body.find('\n');
    auto first_line = trim(body.substr(0, first_newline));
    if (first_line.size() >= 2 && first_line[0] == '#') {
        first_line = trim(first_line.substr(1));
    }
    return first_line;
}

} // anonymous namespace

ParsedSkill parse_skill_content(const std::string& content, const std::string& default_name) {
    ParsedSkill result;
    result.frontmatter.name = default_name;

    // 无 frontmatter：全部为正文
    if (content.empty() || !is_fence(std::string_view(content).substr(0, content.find('\n') == std::string::npos ? content.size() : content.find('\n')))) {
        result.body = content;
        result.frontmatter.description = derive_description(result.body);
        return result;
    }

    // 分离 frontmatter 块与正文
    std::string_view front_part;
    std::string_view body_part;
    {
        std::string_view rest = content;
        // 跳过第一行（---）
        const size_t first_nl = rest.find('\n');
        rest = first_nl == std::string_view::npos ? std::string_view{} : rest.substr(first_nl + 1);

        // 找第二个 --- 行
        const size_t second_fence = rest.find("\n---");
        if (second_fence == std::string_view::npos) {
            // 未闭合：整个文档视为正文，无 frontmatter 字段
            result.body = content;
            result.frontmatter.description = derive_description(result.body);
            return result;
        }
        front_part = rest.substr(0, second_fence);
        size_t body_start = second_fence + 4;  // 跳过 "\n---"
        if (body_start < rest.size() && rest[body_start] == '\r') ++body_start;  // CRLF
        if (body_start < rest.size() && rest[body_start] == '\n') ++body_start;
        body_part = rest.substr(body_start);
    }

    result.body = std::string(body_part);

    // 逐行解析 key: value
    size_t pos = 0;
    while (pos <= front_part.size()) {
        const size_t nl = front_part.find('\n', pos);
        const auto line = front_part.substr(pos, nl == std::string_view::npos ? front_part.size() - pos : nl - pos);
        pos = nl == std::string_view::npos ? front_part.size() + 1 : nl + 1;

        const auto trimmed = trim(line);
        if (trimmed.empty() || trimmed.front() == '#') continue;

        const size_t colon = trimmed.find(':');
        if (colon == std::string::npos) continue;

        std::string key = trimmed.substr(0, colon);
        for (auto& c : key) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        key = trim(key);
        const std::string value = trim(trimmed.substr(colon + 1));

        if (key == "name") {
            if (!value.empty()) result.frontmatter.name = value;
        } else if (key == "description") {
            result.frontmatter.description = value;
        } else if (key == "aliases") {
            result.frontmatter.aliases = parse_aliases(value);
        } else if (key == "argument_hint") {
            result.frontmatter.argument_hint = value;
        } else if (key == "when_to_use") {
            result.frontmatter.when_to_use = value;
        } else if (key == "model") {
            result.frontmatter.model = value;
        } else if (key == "user_invocable") {
            result.frontmatter.user_invocable = parse_bool(value, true);
        } else if (key == "disable_model_invocation") {
            result.frontmatter.disable_model_invocation = parse_bool(value, false);
        }
        // 未知字段忽略（扩展点：paths/hooks 等一期不做）
    }

    if (result.frontmatter.description.empty()) {
        result.frontmatter.description = derive_description(result.body);
    }
    return result;
}

} // namespace agent::skill
