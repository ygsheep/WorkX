/**
 * @file path_validator.cpp
 * @brief 统一路径安全校验实现
 * @details 覆盖 #34 期望的安全机制：
 *          - CWD 边界强制（canonical 解析 symlink 后双重检查）
 *          - 敏感文件/目录硬编码拦截
 *          - Windows 可疑路径模式（UNC / 8.3 / ADS / 长路径前缀 / 尾点空格）
 * @version 1.0.0
 * @date 2026-08
 */

#include "agent/tool/path_validator.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <unordered_set>

#include "core/utils/error.h"

namespace agent::tool {

namespace fs = std::filesystem;

namespace {

/// @brief 路径段小写化（Windows/大小写不敏感文件系统的归一化防御）
std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// @brief 敏感文件名（命中即拦截）
const std::unordered_set<std::string>& sensitive_files() {
    static const std::unordered_set<std::string> k = {
        ".env", ".gitconfig", ".git-credentials", ".netrc", ".npmrc", ".pypirc",
        ".bashrc", ".bash_profile", ".zshrc", ".zprofile", ".profile",
        "id_rsa", "id_dsa", "id_ecdsa", "id_ed25519",
        "authorized_keys", "known_hosts", "credentials", "passwd", "shadow",
        "sam", "ntuser.dat",
    };
    return k;
}

/// @brief 敏感目录段（路径任一目录段命中即拦截）
const std::unordered_set<std::string>& sensitive_dirs() {
    static const std::unordered_set<std::string> k = {
        ".ssh", ".gnupg", ".aws", ".azure", ".gcloud", ".kube",
        "hooks",  // .git/hooks（由 .git 段限定，此处宽松拦截 git/hooks 均拒）
        "config",  // ~/.config 下敏感项（ssh 等已单列；.config 整体拦截偏差可后续收窄）
    };
    return k;
}

/// @brief 文件是否命中敏感清单（含 .env.xxx 前缀变体）
bool is_sensitive_filename(std::string_view filename) {
    const std::string name = lower(filename);
    if (sensitive_files().count(name) > 0) return true;
    // .env.local / .env.production 等变体
    if (name.rfind(".env.", 0) == 0) return true;
    return false;
}

/// @brief 提取最后一个文件名段（末尾含 / 时取前一段）
std::string filename_segment(std::string_view path) {
    std::string_view p = path;
    // 去掉尾部分隔符
    while (!p.empty() && (p.back() == '/' || p.back() == '\\')) {
        p.remove_suffix(1);
    }
    const size_t pos = p.find_last_of("/\\");
    return std::string(pos == std::string_view::npos ? p : p.substr(pos + 1));
}

/// @brief 检查路径中任一目录段命中敏感目录（最后一个段按文件名检查）
bool path_contains_sensitive(std::string_view path) {
    std::string cur;
    for (char c : path) {
        if (c == '/' || c == '\\') {
            if (!cur.empty() && sensitive_dirs().count(lower(cur)) > 0) return true;
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    // 最后一个段按文件名规则检查（同时检查目录命中）
    if (!cur.empty()) {
        if (sensitive_dirs().count(lower(cur)) > 0) return true;
        if (is_sensitive_filename(cur)) return true;
    }
    // Windows 盘符 C: 单独出现时不是文件段
    return false;
}

/// @brief 8.3 短名检测（GIT~1 / BASHR~1 模式：8 字符内 + ~ + 数字）
bool is_short_name(std::string_view seg) {
    // 段必须含 ~，~ 后为数字，~ 前为非 ~ 字符
    const size_t tilde = seg.find('~');
    if (tilde == std::string_view::npos) return false;
    if (tilde == 0) return false;
    // ~ 后全是数字
    for (size_t i = tilde + 1; i < seg.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(seg[i]))) return false;
    }
    return true;
}

} // namespace

bool has_suspicious_windows_pattern(std::string_view path) {
    if (path.empty()) return false;

    // 1. 本地设备长路径前缀（\\?\C:\...）— 归一化防御，拒绝
    if (path.rfind("\\\\?\\", 0) == 0 || path.rfind("//?/", 0) == 0) return true;

    // 2. UNC 路径（\\host\share 或 //host/share）— 可能触发 NTLM 凭据泄露
    if (path.rfind("\\\\", 0) == 0 || path.rfind("//", 0) == 0) return true;

    // 3. 逐段检查：ADS / 8.3 短名 / 尾点空格
    std::string cur;
    for (size_t i = 0; i <= path.size(); ++i) {
        const char c = (i < path.size()) ? path[i] : '\0';
        if (c == '/' || c == '\\' || c == '\0') {
            if (!cur.empty()) {
                // 3a. NTFS 备用数据流（file::$DATA / file:stream）
                if (cur.find("::") != std::string::npos) return true;
                // 3b. 8.3 短名
                if (is_short_name(cur)) return true;
                // 3c. 尾点/尾空格（Windows 解析时剥离，可绕过扩展名/敏感拦截）
                const char last = cur.back();
                if (last == '.' || last == ' ') return true;
            }
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    return false;
}

bool matches_sensitive_path(std::string_view path) {
    return path_contains_sensitive(path);
}

bool is_within_allowed_root(
    std::string_view path,
    std::string_view cwd,
    const std::vector<std::string>& allowlist
) {
    auto under = [](std::string_view p, std::string_view root) {
        if (root.empty()) return false;
        if (p == root) return true;
        if (p.size() > root.size() && p.compare(0, root.size(), root) == 0) {
            const char next = p[root.size()];
            return next == '/' || next == '\\';
        }
        return false;
    };
    if (under(path, cwd)) return true;
    for (const auto& root : allowlist) {
        if (under(path, root)) return true;
    }
    return false;
}

ResultV2<void> validate_path_access(
    std::string_view path,
    std::string_view cwd,
    const std::vector<std::string>& allowlist
) {
    if (path.empty()) {
        return ResultV2<void>::err(Error::Code::PermissionDenied,
                                   "Path is empty");
    }
    if (has_suspicious_windows_pattern(path)) {
        return ResultV2<void>::err(
            Error::Code::PermissionDenied,
            "Path contains a suspicious Windows pattern (UNC / short name / ADS / "
            "long-path prefix / trailing dot-space) and is blocked");
    }
    if (matches_sensitive_path(path)) {
        return ResultV2<void>::err(
            Error::Code::PermissionDenied,
            "Path matches a sensitive file/directory (e.g. .env, .ssh, credentials) "
            "and is blocked");
    }

    // 边界检查：存在时 canonical（解析 symlink），不存在时 weakly_canonical。
    // 两个结果都校验：symlink 指向边界外的场景由 canonical 结果拦截。
    const fs::path p(path);
    std::error_code ec;
    const fs::path canonical = fs::canonical(p, ec);
    if (!ec) {
        if (!is_within_allowed_root(canonical.generic_string(), cwd, allowlist)) {
            return ResultV2<void>::err(
                Error::Code::PermissionDenied,
                "Path is outside the allowed working directory");
        }
        return ResultV2<void>::ok();
    }
    ec.clear();
    const fs::path weakly = fs::weakly_canonical(p, ec);
    if (!ec && !is_within_allowed_root(weakly.generic_string(), cwd, allowlist)) {
        return ResultV2<void>::err(
            Error::Code::PermissionDenied,
            "Path escapes the allowed working directory");
    }
    return ResultV2<void>::ok();
}

} // namespace agent::tool