#pragma once

#include <string>
#include <vector>
#include <utility>
#include <functional>
#include <memory>
#include <chrono>

#include "core/utils/result.h"

namespace workx {

struct ParsedUrl {
    std::string scheme;
    std::string host;
    std::string port;
    std::string target;
};

struct HttpResponse {
    unsigned int status_code = 0;
    std::string body;
    std::string error;
};

class SSEStreamReader;

class HttpClient {
public:
    HttpClient();
    ~HttpClient();

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;

    HttpResponse get(const std::string& url,
                     const std::vector<std::pair<std::string, std::string>>& headers,
                     int timeout_ms = 15000);

    void async_post_stream(const std::string& url,
                           const std::vector<std::pair<std::string, std::string>>& headers,
                           const std::string& body,
                           std::shared_ptr<SSEStreamReader> reader,
                           std::function<void()> on_complete,
                           int timeout_ms = 30000) const;

    void cancel_stream(SSEStreamReader *reader);

    void cancel_stream(const SSEStreamReader* reader) const;

    void shutdown();

    static ParsedUrl parse_url(const std::string& url);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace workx