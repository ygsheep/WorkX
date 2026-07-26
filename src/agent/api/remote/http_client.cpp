#include "agent/api/remote/http_client.h"
#include "agent/api/remote/sse_stream_reader.h"

#include <curl/curl.h>

#include <unordered_map>
#include <atomic>
#include <thread>
#include <mutex>
#include <algorithm>

#include "liblogger/logger.h"

namespace agent {

// ============================================================
// Curl 全局初始化
// ============================================================

static struct CurlGlobal {
    CurlGlobal() { curl_global_init(CURL_GLOBAL_ALL); }
    ~CurlGlobal() { curl_global_cleanup(); }
} s_curl_global;

// ============================================================
// URL 解析
// ============================================================

ParsedUrl HttpClient::parse_url(const std::string& url) {
    ParsedUrl result;

    CURLU* hurl = curl_url();
    if (hurl && curl_url_set(hurl, CURLUPART_URL, url.c_str(), 0) == CURLUE_OK) {
        auto get_part = [&](CURLUPart p) -> std::string {
            char* c = nullptr;
            if (curl_url_get(hurl, p, &c, 0) == CURLUE_OK && c) {
                std::string s(c); curl_free(c); return s;
            }
            return {};
        };
        result.scheme = get_part(CURLUPART_SCHEME);
        result.host   = get_part(CURLUPART_HOST);
        result.port   = get_part(CURLUPART_PORT);
        if (result.port.empty())
            result.port = (result.scheme == "https") ? "443" : "80";
        {
            std::string p = get_part(CURLUPART_PATH);
            if (p.empty()) p = "/";
            std::string q = get_part(CURLUPART_QUERY);
            result.target = q.empty() ? p : p + "?" + q;
        }
        curl_url_cleanup(hurl);
        return result;
    }
    if (hurl) curl_url_cleanup(hurl);

    // fallback
    std::string_view sv(url);
    auto sep = sv.find("://");
    if (sep != std::string_view::npos) {
        result.scheme = sv.substr(0, sep);
        sv.remove_prefix(sep + 3);
    } else {
        result.scheme = "https";
    }
    auto slash = sv.find('/');
    auto auth  = (slash != std::string_view::npos)
                     ? std::string(sv.substr(0, slash)) : std::string(sv);
    result.target = (slash != std::string_view::npos)
                        ? std::string(sv.substr(slash)) : "/";
    auto colon = auth.find(':');
    if (colon != std::string::npos) {
        result.host = auth.substr(0, colon);
        result.port = auth.substr(colon + 1);
    } else {
        result.host = auth;
        result.port = (result.scheme == "https") ? "443" : "80";
    }
    return result;
}

// ============================================================
// 写入回调（同步 GET）
// ============================================================

static size_t write_cb(void* ptr, size_t size, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(
        static_cast<const char*>(ptr), size * nmemb);
    return size * nmemb;
}

// ============================================================
// Sync GET
// ============================================================

HttpResponse HttpClient::get(const std::string& url,
                             const std::vector<std::pair<std::string, std::string>>& headers,
                             int timeout_ms) {
    HttpResponse resp;
    LOG_DEBUG("[http] GET {} timeout={}ms", url, timeout_ms);
    CURL* curl = curl_easy_init();
    if (!curl) {
        resp.error = "curl init failed";
        LOG_ERROR("[http] GET {} curl_easy_init failed", url);
        return resp;
    }

    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms));
    // C.7：补连接超时，防止 DNS/TCP 阶段挂死耗尽总超时
    // 默认 10s 连接超时（若 timeout_ms < 10s 则跟随总超时）
    long connect_to = (std::min)(10000L, static_cast<long>(timeout_ms));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, connect_to);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

    struct curl_slist* hl = nullptr;
    for (const auto& [k, v] : headers)
        hl = curl_slist_append(hl, (k + ": " + v).c_str());
    if (hl) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hl);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        resp.error = curl_easy_strerror(rc);
        LOG_ERROR("[http] GET {} failed: {} (rc={})", url, resp.error, static_cast<int>(rc));
    } else {
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        resp.status_code = static_cast<unsigned int>(code);
        resp.body = std::move(body);
        if (code >= 400) {
            LOG_WARN("[http] GET {} returned HTTP {} body_len={}", url, code, resp.body.size());
        } else {
            LOG_DEBUG("[http] GET {} -> {} bytes={}", url, code, resp.body.size());
        }
    }
    if (hl) curl_slist_free_all(hl);
    curl_easy_cleanup(curl);
    return resp;
}

// ============================================================
// StreamSession — 异步流式 POST 会话
// ============================================================

class StreamSession : public std::enable_shared_from_this<StreamSession> {
public:
    CURL* easy_handle() const { return m_curl; }

    StreamSession(const ParsedUrl& url,
                 const std::vector<std::pair<std::string, std::string>>& headers,
                 const std::string& body,
                 std::shared_ptr<SSEStreamReader> reader,
                 std::function<void()> on_complete,
                 const int timeout_ms)
        : m_body(body)
        , m_reader(std::move(reader))
        , m_on_complete(std::move(on_complete))
    {
        m_curl = curl_easy_init();
        if (!m_curl) return;

        std::string full_url = url.scheme + "://" + url.host;
        if ((url.scheme == "https" && url.port != "443") ||
            (url.scheme == "http"  && url.port != "80"))
            full_url += ":" + url.port;
        full_url += url.target;

        curl_easy_setopt(m_curl, CURLOPT_URL, full_url.c_str());
        curl_easy_setopt(m_curl, CURLOPT_POST, 1L);
        curl_easy_setopt(m_curl, CURLOPT_POSTFIELDS, m_body.c_str());
        curl_easy_setopt(m_curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(m_body.size()));
        curl_easy_setopt(m_curl, CURLOPT_WRITEFUNCTION, &StreamSession::stream_write_cb);
        curl_easy_setopt(m_curl, CURLOPT_WRITEDATA, this);
        // 流式传输用空闲超时（无数据传输 N 秒则断开），不用总时长超时
        // 长响应（如 reasoning model 60s+）正常，但断网 N 秒会触发断开
        // CURLOPT_LOW_SPEED_LIMIT + CURLOPT_LOW_SPEED_TIME:
        //   若传输速度低于 LOW_SPEED_LIMIT bytes/s 持续 LOW_SPEED_TIME 秒，则断开
        // 用括号包裹 std::max 防止 windows.h 的 max 宏展开
        const long idle_seconds = (std::max)(1L, static_cast<long>(timeout_ms / 1000));
        curl_easy_setopt(m_curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
        curl_easy_setopt(m_curl, CURLOPT_LOW_SPEED_TIME, idle_seconds);
        // 保留连接超时（10 秒）
        curl_easy_setopt(m_curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
        curl_easy_setopt(m_curl, CURLOPT_FOLLOWLOCATION, 0L);
        curl_easy_setopt(m_curl, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(m_curl, CURLOPT_SSL_VERIFYHOST, 2L);
        curl_easy_setopt(m_curl, CURLOPT_NOSIGNAL, 1L);

        for (const auto& [k, v] : headers)
            m_header_list = curl_slist_append(m_header_list, (k + ": " + v).c_str());
        if (m_header_list)
            curl_easy_setopt(m_curl, CURLOPT_HTTPHEADER, m_header_list);
    }

    ~StreamSession() {
        if (m_curl) {
            // 防御：m_multi 可能已被 HttpClient 析构清理为 nullptr
            // （shutdown 路径会先 curl_multi_cleanup 再触发 StreamSession 析构）
            if (m_added_to_multi.load() && m_multi) {
                curl_multi_remove_handle(m_multi, m_curl);
            }
            curl_easy_cleanup(m_curl);
        }
        if (m_header_list) curl_slist_free_all(m_header_list);
    }

    void run(CURLM* multi) {
        if (!m_curl) { finish("curl init failed"); return; }
        m_multi = multi;
        m_added_to_multi.store(true);
        curl_multi_add_handle(m_multi, m_curl);
    }

    /// @brief 立即取消会话：设置标志 + 从 multi 移除 handle，避免依赖下一次 write 回调
    /// @details 原实现只设原子标志，需等下次 write 回调返回 0 才真正停止；
    ///          若网络已断开 write 回调不再触发，cancel 不生效。
    ///          现在直接 remove_handle 让 libcurl 立即停止传输。
    void cancel() {
        m_cancelled.store(true);
        if (m_added_to_multi.load() && m_multi) {
            curl_multi_remove_handle(m_multi, m_curl);
            m_added_to_multi.store(false);
        }
    }

    void on_transfer_done(const CURLcode code) {
        if (m_cancelled.load()) {
            LOG_DEBUG("[http][stream] transfer cancelled");
            finish("");
            return;
        }
        if (code != CURLE_OK) {
            std::string err = curl_easy_strerror(code);
            LOG_ERROR("[http][stream] transfer failed: {} (rc={})", err, static_cast<int>(code));
            finish(err);
            return;
        }

        // 检查 HTTP 状态码
        long http_code = 0;
        curl_easy_getinfo(m_curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code >= 400) {
            // 4xx/5xx：附加已累积的响应体作为错误信息一部分
            // （stream_write_cb 在 http_code >= 400 时累积到 m_error_body 而非喂给 reader）
            std::string err_msg = std::string("HTTP error: ") + std::to_string(http_code);
            if (!m_error_body.empty()) {
                // 截断防止过长（最多 500 字符）
                err_msg += " - " + m_error_body.substr(0, 500);
            }
            LOG_WARN("[http][stream] HTTP {} error_body_len={}", http_code, m_error_body.size());
            finish(err_msg);
            return;
        }
        LOG_DEBUG("[http][stream] transfer done HTTP {}", http_code);
        if (m_reader && !m_reader->is_finished())
            m_reader->finish();
        if (m_on_complete) m_on_complete();
    }

    // 主动以错误完成会话（供 HttpClient::async_post_stream 在 curl 初始化失败时调用）
    void finish_with_error(const std::string& err) { finish(err); }

private:
    void finish(const std::string& err) const {
        if (m_reader && !m_reader->is_finished())
            m_reader->finish(err);
        if (m_on_complete) m_on_complete();
    }

    static size_t stream_write_cb(void* ptr, size_t size, size_t nmemb, void* userdata) {
        auto* self = static_cast<StreamSession*>(userdata);
        if (self->m_cancelled.load()) return 0;

        // HTTP 错误响应（4xx/5xx）：累积 body 到 m_error_body 作为错误信息
        // 正常响应（2xx）：喂给 reader 进行 SSE 解析
        long http_code = 0;
        curl_easy_getinfo(self->m_curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code >= 400) {
            self->m_error_body.append(static_cast<const char*>(ptr), size * nmemb);
            return size * nmemb;
        }

        if (self->m_reader)
            // C.10：直接传 string_view，避免每个 SSE chunk 都构造 std::string 拷贝
            self->m_reader->feed_data(std::string_view(
                static_cast<const char*>(ptr), size * nmemb));
        return size * nmemb;
    }

    CURL* m_curl = nullptr;
    CURLM* m_multi = nullptr;
    std::atomic<bool> m_added_to_multi{false};
    struct curl_slist* m_header_list = nullptr;
    std::string m_body;
    std::string m_error_body;  // 4xx/5xx 响应体累积
    std::shared_ptr<SSEStreamReader> m_reader;
    std::function<void()> m_on_complete;
    std::atomic<bool> m_cancelled{false};
};

// ============================================================
// HttpClient::Impl
// ============================================================

struct HttpClient::Impl {
    CURLM* multi;
    std::mutex sessions_mutex;
    std::unordered_map<CURL*, std::shared_ptr<StreamSession>> sessions_by_handle;
    std::unordered_map<SSEStreamReader*, std::weak_ptr<StreamSession>> sessions_by_reader;
    std::thread poll_thread;
    std::atomic<bool> running{true};

    Impl() {
        multi = curl_multi_init();
        poll_thread = std::thread([this]() { poll_loop(); });
    }

    void poll_loop() {
        while (running.load()) {
            int still_running = 0;
            curl_multi_perform(multi, &still_running);

            int msgs_left = 0;
            CURLMsg* msg;
            while ((msg = curl_multi_info_read(multi, &msgs_left)) != nullptr) {
                if (msg->msg == CURLMSG_DONE) {
                    std::shared_ptr<StreamSession> session;
                    {
                        std::lock_guard<std::mutex> lock(sessions_mutex);
                        auto it = sessions_by_handle.find(msg->easy_handle);
                        if (it != sessions_by_handle.end()) {
                            session = it->second;
                            sessions_by_handle.erase(it);
                        }
                        // 也从 reader map 清理（C.4：完善 sessions_by_reader 清理路径）
                        // expired 的 weak_ptr 在此统一回收，避免长生命周期下 map 无限增长
                        for (auto rit = sessions_by_reader.begin(); rit != sessions_by_reader.end(); ) {
                            if (rit->second.expired()) rit = sessions_by_reader.erase(rit);
                            else ++rit;
                        }
                    }
                    if (session)
                        session->on_transfer_done(msg->data.result);
                }
            }

            // C.3：用 curl_multi_wait 替代 sleep_for(10ms) 忙等
            // curl_multi_wait 内部用 select/poll，无传输时阻塞等待（最多 100ms），
            // CPU 占用近零；有数据时立即返回。libcurl 官方推荐 API。
            // 即便 still_running == 0 也用 curl_multi_wait 短轮询，避免空转
            curl_multi_wait(multi, nullptr, 0, 100, nullptr);
        }
    }

    ~Impl() { shutdown(); }

    void shutdown() {
        running.store(false);
        if (poll_thread.joinable()) poll_thread.join();
        {
            std::lock_guard<std::mutex> lock(sessions_mutex);
            sessions_by_handle.clear();
            sessions_by_reader.clear();
        }
        if (multi) { curl_multi_cleanup(multi); multi = nullptr; }
    }
};

// ============================================================
// HttpClient 接口
// ============================================================

HttpClient::HttpClient()
    : m_impl(std::make_unique<Impl>()) {}

HttpClient::~HttpClient() { shutdown(); }

void HttpClient::async_post_stream(
        const std::string& url,
        const std::vector<std::pair<std::string, std::string>>& headers,
        const std::string& body,
        std::shared_ptr<SSEStreamReader> reader,
        std::function<void()> on_complete,
        int timeout_ms) const {
    LOG_DEBUG("[http][stream] POST {} body_len={} timeout={}ms", url, body.size(), timeout_ms);
    auto parsed = parse_url(url);
    auto* key = reader.get();

    const auto session = std::make_shared<StreamSession>(
        parsed, headers, body, reader, std::move(on_complete), timeout_ms);

    if (!session->easy_handle()) {
        // curl 初始化失败：主动 finish reader 让上层能收到错误，避免 next() 无限阻塞
        // （调用方已把 reader 存入 m_active_reader，若不 finish 会永远等数据）
        // finish_with_error 会同时触发 reader->finish 和 on_complete 回调
        LOG_ERROR("[http][stream] POST {} curl_easy_init failed", url);
        session->finish_with_error("Failed to initialize curl session");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_impl->sessions_mutex);
        m_impl->sessions_by_handle[session->easy_handle()] = session;
        m_impl->sessions_by_reader[key] = session;
    }

    session->run(m_impl->multi);
}

void HttpClient::cancel_stream(SSEStreamReader* reader) {
    LOG_DEBUG("[http][stream] cancel_stream requested");
    std::shared_ptr<StreamSession> session;
    {
        std::lock_guard<std::mutex> lock(m_impl->sessions_mutex);
        auto it = m_impl->sessions_by_reader.find(reader);
        if (it != m_impl->sessions_by_reader.end()) {
            session = it->second.lock();
            if (session) {
                session->cancel();
                auto hit = m_impl->sessions_by_handle.find(session->easy_handle());
                if (hit != m_impl->sessions_by_handle.end())
                    m_impl->sessions_by_handle.erase(hit);
            }
            m_impl->sessions_by_reader.erase(it);
        }
    }
    // session 彻底移除后，安全触发完成回调
    if (session)
        session->on_transfer_done(CURLE_OK);
}

void HttpClient::shutdown() {
    if (m_impl) m_impl->shutdown();
}

} // namespace agent