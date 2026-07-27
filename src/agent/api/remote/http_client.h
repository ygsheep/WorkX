#pragma once

#include <string>
#include <vector>
#include <utility>
#include <functional>
#include <memory>
#include <chrono>

#include "core/utils/result.h"          // 旧 Result（过渡期保留）
#include "core/utils/result_v2.h"       // V2-2：新 ResultV2
#include "core/utils/error.h"           // V2-2：Error 类型

namespace agent {

struct ParsedUrl {
    std::string scheme;
    std::string host;
    std::string port;
    std::string target;
};

/// @brief HTTP 响应（E-6：错误码分类清晰化）
/// @details
/// 字段语义：
/// - `status_code`：HTTP 状态码；网络错误时为 0
/// - `body`：响应体（即使 4xx/5xx 也可能有 body）
/// - `error`：curl/网络错误描述；HTTP 错误时为空
///
/// 三种状态判断：
/// | 状态         | status_code | error    | 便捷方法              |
/// |--------------|-------------|----------|-----------------------|
/// | 成功 (2xx)   | 200-299     | empty    | is_success()          |
/// | HTTP 错误    | 4xx/5xx     | empty    | is_http_error()       |
/// | 网络错误     | 0           | non-empty| is_network_error()    |
struct HttpResponse {
    unsigned int status_code = 0;
    std::string body;
    std::string error;

    /// @brief 是否成功（HTTP 2xx）
    bool is_success() const noexcept {
        return error.empty() && status_code >= 200 && status_code <= 299;
    }

    /// @brief 是否 HTTP 错误（4xx/5xx，curl 无错误）
    bool is_http_error() const noexcept {
        return error.empty() && status_code >= 400 && status_code <= 599;
    }

    /// @brief 是否网络错误（curl 失败，无 HTTP 响应）
    bool is_network_error() const noexcept {
        return !error.empty() && status_code == 0;
    }

    /// @brief 是否客户端错误（4xx）
    bool is_client_error() const noexcept {
        return error.empty() && status_code >= 400 && status_code <= 499;
    }

    /// @brief 是否服务端错误（5xx，可重试）
    bool is_server_error() const noexcept {
        return error.empty() && status_code >= 500 && status_code <= 599;
    }

    /// @brief 是否限流（429 Too Many Requests）
    bool is_rate_limited() const noexcept {
        return error.empty() && status_code == 429;
    }

    /// @brief 是否可重试错误（与 HttpRetryPolicy.is_retryable 配对）
    /// @details 429 + 5xx + 网络错误 可重试；4xx（除 429）不可重试
    bool is_retryable() const noexcept {
        return is_rate_limited() || is_server_error() || is_network_error();
    }
};

class SSEStreamReader;

class HttpClient {
public:
    HttpClient();
    ~HttpClient();

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    /// @brief 同步 GET 请求
    /// @return 成功返回 HttpResponse（含 2xx/4xx/5xx 状态码）；
    ///         失败返回 Error（仅限网络错误，HTTP 4xx/5xx 仍通过 HttpResponse 返回）
    /// @details V2-2：签名从 HttpResponse 改为 ResultV2<HttpResponse>，
    ///          网络错误（curl 失败、无法到达服务器）通过 Error 携带错误码；
    ///          HTTP 4xx/5xx 仍通过 HttpResponse 返回，调用方可通过 is_http_error() 判断
    ResultV2<HttpResponse> get(const std::string& url,
                               const std::vector<std::pair<std::string, std::string>>& headers,
                               int timeout_ms = 15000);

    void async_post_stream(const std::string& url,
                           const std::vector<std::pair<std::string, std::string>>& headers,
                           const std::string& body,
                           std::shared_ptr<SSEStreamReader> reader,
                           std::function<void()> on_complete,
                           int timeout_ms = 30000) const;

    void cancel_stream(SSEStreamReader *reader);

    void shutdown();

    static ParsedUrl parse_url(const std::string& url);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace agent