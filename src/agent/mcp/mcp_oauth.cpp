/**
 * @file mcp_oauth.cpp
 * @brief OAuth 2.0 客户端实现（Issue #27 M4）
 * @details
 * - client_credentials：POST token_endpoint（grant_type=client_credentials）
 * - authorization_code + PKCE：build_authorization_url → 回环回调收 code →
 *   exchange_code（grant_type=authorization_code + code_verifier）
 * - refresh：grant_type=refresh_token 自动续期
 * - 回环回调：Winsock/POSIX 原始 socket 监听 127.0.0.1 一次 HTTP GET
 * @version 1.0.0
 * @date 2026-08
 */

#include "agent/mcp/mcp_oauth.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <random>

#include "agent/compact/sha256.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace agent::mcp {

namespace {

/// @brief 从 JSON 响应解析 token 字段并填充成员
/// @return ok: 已填充；err: 响应缺少 access_token
ResultV2<void> parse_token_response(McpOAuthClient* /*unused*/,
                                    const nlohmann::json& j,
                                    std::string& access_token,
                                    std::string& refresh_token,
                                    std::chrono::steady_clock::time_point& expires_at) {
    if (!j.is_object() || !j.contains("access_token")) {
        return ResultV2<void>::err(Error::Code::InvalidInput,
            "OAuth token 响应缺少 access_token",
            "body=" + (j.is_object() ? j.dump() : std::string("<non-object>")));
    }
    access_token = j.at("access_token").get<std::string>();
    refresh_token = j.value("refresh_token", "");
    const int64_t expires_in = j.value("expires_in", 3600);
    expires_at = std::chrono::steady_clock::now() +
                 std::chrono::seconds(expires_in > 0 ? expires_in : 3600);
    return ResultV2<void>::ok();
}

} // anonymous namespace

ResultV2<void> McpOAuthClient::configure(const McpOAuthConfig& cfg) {
    if (!cfg.valid()) {
        return ResultV2<void>::err(Error::Code::ConfigInvalid,
            "OAuth 配置无效：需要 tokenEndpoint + clientId"
            "（authorization_code 还需 authEndpoint）",
            "McpOAuthClient::configure");
    }
    m_cfg = cfg;
    m_configured = true;
    // 重新配置后失效旧 token
    m_access_token.clear();
    m_refresh_token.clear();
    return ResultV2<void>::ok();
}

ResultV2<std::string> McpOAuthClient::access_token() {
    if (!m_configured) {
        return ResultV2<std::string>::err(Error::Code::ConfigInvalid,
            "OAuth 未配置", "McpOAuthClient::access_token");
    }
    const auto now = std::chrono::steady_clock::now();
    if (!m_access_token.empty() && m_expires_at > now) {
        return ResultV2<std::string>::ok(m_access_token);
    }
    // 有 refresh_token 先尝试刷新，否则按流程重新获取
    if (!m_refresh_token.empty()) {
        auto r = refresh();
        if (r.is_ok()) return ResultV2<std::string>::ok(m_access_token);
        // 刷新失败（refresh_token 失效）→ 回退重新走完整流程
    }
    if (m_cfg.flow == "client_credentials") {
        auto r = fetch_token("client_credentials", {});
        if (r.is_err()) return r.error();
        return ResultV2<std::string>::ok(m_access_token);
    }
    // authorization_code：无有效 token，需重新授权
    return ResultV2<std::string>::err(Error::Code::PermissionDenied,
        "OAuth 需要重新授权（authorization_code 流程）",
        "McpOAuthClient::access_token");
}

ResultV2<std::string> McpOAuthClient::authorization_header() {
    auto tok = access_token();
    if (tok.is_err()) return tok.error();
    return ResultV2<std::string>::ok("Bearer " + tok.value());
}

ResultV2<std::string> McpOAuthClient::build_authorization_url() {
    if (!m_configured || m_cfg.flow != "authorization_code") {
        return ResultV2<std::string>::err(Error::Code::ConfigInvalid,
            "authorization_code 流程未配置", "McpOAuthClient::build_authorization_url");
    }
    // PKCE：code_verifier（43-128 字符）→ code_challenge = base64url(SHA256(verifier))
    m_code_verifier = random_string(64);
    const std::string challenge = base64url_encode(compact::sha256(m_code_verifier));
    m_state = random_string(24);

    std::string url = m_cfg.auth_endpoint;
    url += (url.find('?') == std::string::npos ? "?" : "&");
    url += "response_type=code";
    url += "&client_id=" + url_encode(m_cfg.client_id);
    url += "&redirect_uri=" + url_encode(m_cfg.redirect_uri);
    url += "&code_challenge=" + url_encode(challenge);
    url += "&code_challenge_method=S256";
    url += "&state=" + url_encode(m_state);
    if (!m_cfg.scopes.empty()) {
        std::string scope;
        for (const auto& s : m_cfg.scopes) {
            if (!scope.empty()) scope += " ";
            scope += s;
        }
        url += "&scope=" + url_encode(scope);
    }
    return ResultV2<std::string>::ok(std::move(url));
}

ResultV2<void> McpOAuthClient::exchange_code(const std::string& code) {
    if (!m_configured || m_cfg.flow != "authorization_code") {
        return ResultV2<void>::err(Error::Code::ConfigInvalid,
            "authorization_code 流程未配置", "McpOAuthClient::exchange_code");
    }
    return fetch_token("authorization_code", {
        {"code", code},
        {"redirect_uri", m_cfg.redirect_uri},
        {"code_verifier", m_code_verifier},
    });
}

// ============================================================
// 内部
// ============================================================

ResultV2<void> McpOAuthClient::fetch_token(
    const std::string& grant_type,
    const std::map<std::string, std::string>& extra) {
    std::string body = "grant_type=" + url_encode(grant_type) +
                       "&client_id=" + url_encode(m_cfg.client_id);
    if (!m_cfg.client_secret.empty()) {
        body += "&client_secret=" + url_encode(m_cfg.client_secret);
    }
    for (const auto& [k, v] : extra) {
        body += "&" + url_encode(k) + "=" + url_encode(v);
    }
    if (!m_cfg.scopes.empty()) {
        std::string scope;
        for (const auto& s : m_cfg.scopes) {
            if (!scope.empty()) scope += " ";
            scope += s;
        }
        body += "&scope=" + url_encode(scope);
    }

    std::vector<std::pair<std::string, std::string>> headers = {
        {"Content-Type", "application/x-www-form-urlencoded"},
        {"Accept", "application/json"},
    };
    auto resp = m_http.post(m_cfg.token_endpoint, headers, body, 15000);
    if (resp.is_err()) return resp.error();
    const auto& r = resp.value();
    if (!r.is_success()) {
        return ResultV2<void>::err(Error::Code::NetworkDisconnected,
            "OAuth token 请求失败: HTTP " + std::to_string(r.status_code),
            "url=" + m_cfg.token_endpoint + "; body=" + r.body.substr(0, 200));
    }
    try {
        const auto j = nlohmann::json::parse(r.body);
        return parse_token_response(this, j, m_access_token, m_refresh_token, m_expires_at);
    } catch (const nlohmann::json::exception&) {
        return ResultV2<void>::err(Error::Code::InvalidInput,
            "OAuth token 响应非 JSON", "body=" + r.body.substr(0, 200));
    }
}

ResultV2<void> McpOAuthClient::refresh() {
    return fetch_token("refresh_token", {
        {"refresh_token", m_refresh_token},
    });
}

std::string McpOAuthClient::url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[(c >> 4) & 0xF];
            out += hex[c & 0xF];
        }
    }
    return out;
}

std::string McpOAuthClient::random_string(size_t n) {
    static const char* chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<size_t> dist(0, std::strlen(chars) - 1);
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) out += chars[dist(gen)];
    return out;
}

std::string McpOAuthClient::base64url_encode(const std::string& in) {
    static const char* table =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    while (i + 3 <= in.size()) {
        const uint32_t n = (static_cast<uint8_t>(in[i]) << 16) |
                           (static_cast<uint8_t>(in[i + 1]) << 8) |
                           static_cast<uint8_t>(in[i + 2]);
        out += table[(n >> 18) & 0x3F];
        out += table[(n >> 12) & 0x3F];
        out += table[(n >> 6) & 0x3F];
        out += table[n & 0x3F];
        i += 3;
    }
    const size_t rem = in.size() - i;
    if (rem == 1) {
        const uint32_t n = static_cast<uint8_t>(in[i]) << 16;
        out += table[(n >> 18) & 0x3F];
        out += table[(n >> 12) & 0x3F];
    } else if (rem == 2) {
        const uint32_t n = (static_cast<uint8_t>(in[i]) << 16) |
                           (static_cast<uint8_t>(in[i + 1]) << 8);
        out += table[(n >> 18) & 0x3F];
        out += table[(n >> 12) & 0x3F];
        out += table[(n >> 6) & 0x3F];
    }
    return out;
}

// ============================================================
// 回环回调监听（authorization_code 流程）
// ============================================================

ResultV2<std::string> oauth_loopback_listen(int port, int& out_port, int timeout_ms) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        return ResultV2<std::string>::err(Error::Code::InternalError,
            "WSAStartup 失败", "oauth_loopback_listen");
    }
    auto cleanup_wsa = [&] { WSACleanup(); };
    const int fd = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
    if (fd == -1) {
        cleanup_wsa();
        return ResultV2<std::string>::err(Error::Code::InternalError,
            "创建 socket 失败", "oauth_loopback_listen");
    }
    auto close_fd = [&] { closesocket(static_cast<SOCKET>(fd)); };
#else
    const int fd = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
    if (fd < 0) {
        return ResultV2<std::string>::err(Error::Code::InternalError,
            "创建 socket 失败", "oauth_loopback_listen");
    }
    auto close_fd = [&] { ::close(fd); };
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close_fd();
#ifdef _WIN32
        cleanup_wsa();
#endif
        return ResultV2<std::string>::err(Error::Code::InternalError,
            "绑定回环端口失败", "oauth_loopback_listen");
    }
    if (listen(fd, 1) != 0) {
        close_fd();
#ifdef _WIN32
        cleanup_wsa();
#endif
        return ResultV2<std::string>::err(Error::Code::InternalError,
            "监听失败", "oauth_loopback_listen");
    }

    // 获取实际端口（port=0 时）
    sockaddr_in bound{};
    socklen_t bound_len = sizeof(bound);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &bound_len) == 0) {
        out_port = ntohs(bound.sin_port);
    }

    // 等待连接（带超时）
    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(static_cast<SOCKET>(fd), &rfds);
    const int sel = select(fd + 1, &rfds, nullptr, nullptr, &tv);
    if (sel <= 0) {
        close_fd();
#ifdef _WIN32
        cleanup_wsa();
#endif
        return ResultV2<std::string>::err(Error::Code::NetworkTimeout,
            "等待 OAuth 回调超时", "oauth_loopback_listen");
    }

#ifdef _WIN32
    const int client = static_cast<int>(accept(fd, nullptr, nullptr));
#else
    const int client = static_cast<int>(accept(fd, nullptr, nullptr));
#endif
    close_fd();
    if (client < 0) {
#ifdef _WIN32
        cleanup_wsa();
#endif
        return ResultV2<std::string>::err(Error::Code::InternalError,
            "接受回调连接失败", "oauth_loopback_listen");
    }
    auto close_client = [&] {
#ifdef _WIN32
        closesocket(static_cast<SOCKET>(client));
#else
        ::close(client);
#endif
    };

    // 读取请求行（GET /callback?code=...&state=... HTTP/1.1）
    std::string request;
    char buf[1024];
    for (int i = 0; i < 32; ++i) {
        const int n = static_cast<int>(recv(static_cast<SOCKET>(client), buf, sizeof(buf), 0));
        if (n <= 0) break;
        request.append(buf, static_cast<size_t>(n));
        if (request.find("\r\n\r\n") != std::string::npos) break;
    }

    // 提取 code 查询参数
    std::string code;
    const auto qpos = request.find('?');
    if (qpos != std::string::npos) {
        const auto sp = request.find(' ', qpos);
        const std::string query = request.substr(qpos + 1, sp == std::string::npos
            ? std::string::npos : sp - qpos - 1);
        // 简单解析 code=<urlencoded>
        const auto cpos = query.find("code=");
        if (cpos != std::string::npos) {
            const auto end = query.find('&', cpos + 5);
            code = query.substr(cpos + 5, end == std::string::npos
                ? std::string::npos : end - cpos - 5);
        }
    }

    // 返回成功页面
    const std::string ok = code.empty()
        ? "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"
        : "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n"
          "Connection: close\r\n\r\n"
          "<html><body><h3>授权成功，可关闭此窗口</h3></body></html>";
#ifdef _WIN32
    send(static_cast<SOCKET>(client), ok.data(), static_cast<int>(ok.size()), 0);
#else
    ::send(client, ok.data(), ok.size(), 0);
#endif
    close_client();
#ifdef _WIN32
    cleanup_wsa();
#endif

    if (code.empty()) {
        return ResultV2<std::string>::err(Error::Code::InvalidInput,
            "OAuth 回调缺少 code 参数", "oauth_loopback_listen");
    }
    return ResultV2<std::string>::ok(std::move(code));
}

} // namespace agent::mcp
