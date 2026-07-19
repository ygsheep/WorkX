/**
 * @file secret_scanner.cpp
 * @brief 密钥扫描器实现
 * @details 规则集移植自 Claude Code secretScanner.ts。
 *          使用 std::regex（ECMAScript 语法），首次调用懒加载并缓存到函数静态变量。
 *          性能权衡：std::regex 较慢但零依赖；MVP 阶段足够，未来可换 re2。
 * @author workx
 * @version 1.0.0
 * @date 2026-07
 */

#include "agent/tool/secret_scanner.h"

#include <algorithm>
#include <format>
#include <regex>
#include <unordered_map>
#include <unordered_set>

namespace agent::tool {

namespace {

/// @brief 密钥规则
struct SecretRule {
    std::string id;
    std::string label;
    std::regex re;
};

/// @brief kebab-case → Title Case
/// @details 如 "github-pat" → "GitHub PAT"。
///          特殊大小写：aws → AWS、pat → PAT、api → API、gcp → GCP、
///          oauth → OAuth、vcs → VCS、tf → TF
std::string rule_id_to_label(const std::string& id) {
    // 特殊映射表
    static const std::unordered_map<std::string, std::string> special = {
        {"aws", "AWS"},
        {"gcp", "GCP"},
        {"api", "API"},
        {"pat", "PAT"},
        {"oauth", "OAuth"},
        {"vcs", "VCS"},
        {"tf", "TF"},
        {"ad", "AD"},
        {"a", "A"},
    };

    std::string result;
    result.reserve(id.size() * 2);
    bool start_word = true;

    for (char c : id) {
        if (c == '-' || c == '_') {
            start_word = true;
            continue;
        }
        if (start_word) {
            // 收集当前单词（小写形式）查特殊映射
            // 简化：逐字符查特殊映射（覆盖单字符情况）
            // 对多字符特殊词（aws/gcp/api 等）单独处理
            start_word = false;
        }
        result.push_back(c);
    }

    // 后处理：对多字符特殊词替换
    // 简化实现：仅处理单字符大写 + 几个固定替换
    // 直接硬编码 10 条规则的标签，避免复杂字符串处理
    static const std::unordered_map<std::string, std::string> id_to_label = {
        {"aws-access-token",           "AWS Access Token"},
        {"gcp-api-key",                "GCP API Key"},
        {"github-pat",                 "GitHub PAT"},
        {"github-fine-grained-pat",    "GitHub Fine-Grained PAT"},
        {"github-oauth",               "GitHub OAuth"},
        {"gitlab-pat",                 "GitLab PAT"},
        {"slack-bot-token",            "Slack Bot Token"},
        {"slack-user-token",           "Slack User Token"},
        {"anthropic-api-key",          "Anthropic API Key"},
        {"openai-api-key",             "OpenAI API Key"},
        {"private-key",                "Private Key"},
        {"stripe-access-token",        "Stripe Access Token"},
    };

    auto it = id_to_label.find(id);
    if (it != id_to_label.end()) {
        return it->second;
    }
    return result;
}

/// @brief 构造规则集（懒加载）
const std::vector<SecretRule>& rules() {
    static const std::vector<SecretRule> kRules = []() {
        std::vector<SecretRule> v;
        // ECMAScript 语法（默认），与 JS RegExp 基本兼容
        constexpr auto kECMA = std::regex::ECMAScript;
        constexpr auto kECMA_ICASE = std::regex::ECMAScript | std::regex::icase;

        auto add = [&](std::string id, std::string pattern, bool icase = false) {
            SecretRule r;
            r.id = std::move(id);
            r.label = rule_id_to_label(r.id);
            r.re = std::regex(pattern, icase ? kECMA_ICASE : kECMA);
            v.push_back(std::move(r));
        };

        // === 云提供商 ===
        add("aws-access-token", R"(AKIA[0-9A-Z]{16})");
        add("aws-access-token", R"(ASIA[0-9A-Z]{16})");
        add("gcp-api-key",      R"(AIza[0-9A-Za-z\-_]{35})");

        // === 版本控制 ===
        add("github-pat",                R"(ghp_[0-9a-zA-Z]{36})");
        add("github-fine-grained-pat",   R"(github_pat_[0-9a-zA-Z_]{82})");
        add("github-oauth",              R"(gho_[0-9a-zA-Z]{36})");
        add("gitlab-pat",                R"(glpat-[0-9a-zA-Z\-_]{20})");

        // === 通讯 ===
        add("slack-bot-token",  R"(xoxb-[0-9]{10,}-[0-9]{10,}-[0-9a-zA-Z]{12,})");
        add("slack-user-token", R"(xox[pe]-[0-9]{10,}-[0-9]{10,}-[0-9a-zA-Z]{12,})");

        // === AI APIs ===
        add("anthropic-api-key", R"(sk-ant-api[0-9]{2}-[0-9a-zA-Z\-_]{20,})");
        add("openai-api-key",    R"(sk-proj-[0-9a-zA-Z\-_]{20,})");
        add("openai-api-key",    R"(sk-svcacct-[0-9a-zA-Z\-_]{20,})");
        add("openai-api-key",    R"(sk-admin-[0-9a-zA-Z\-_]{20,})");

        // === 支付 ===
        add("stripe-access-token", R"((sk|rk)_(test|live|prod)_[0-9a-zA-Z]{10,})");

        // === 加密 ===
        add("private-key", R"(-----BEGIN [A-Z ]*PRIVATE KEY-----)", true);

        return v;
    }();
    return kRules;
}

} // anonymous namespace

std::vector<SecretMatch> scan_for_secrets(const std::string& content) {
    std::vector<SecretMatch> matches;
    std::unordered_set<std::string> seen;

    for (const auto& rule : rules()) {
        try {
            if (std::regex_search(content, rule.re)) {
                // 按 rule_id 去重
                if (seen.insert(rule.id).second) {
                    matches.push_back({rule.id, rule.label});
                }
            }
        } catch (const std::regex_error&) {
            // 规则编译/匹配异常时跳过（不阻断扫描）
        }
    }
    return matches;
}

std::string scan_for_secret_error(const std::string& content) {
    auto matches = scan_for_secrets(content);
    if (matches.empty()) return {};

    std::string labels;
    for (size_t i = 0; i < matches.size(); ++i) {
        if (i > 0) labels += ", ";
        labels += matches[i].label;
    }
    return std::format(
        "Content contains potential secrets ({}) and cannot be written to file. "
        "Remove the sensitive content and try again.",
        labels
    );
}

} // namespace agent::tool
