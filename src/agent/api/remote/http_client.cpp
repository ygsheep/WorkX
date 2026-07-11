#include "agent/api/remote/http_client.h"
#include "agent/api/remote/sse_stream_reader.h"

#include <curl/curl.h>

#include <unordered_map>
#include <atomic>
#include <thread>
#include <mutex>

namespace workx {

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
    CURL* curl = curl_easy_init();
    if (!curl) { resp.error = "curl init failed"; return resp; }

    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms));
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
    } else {
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        resp.status_code = static_cast<unsigned int>(code);
        resp.body = std::move(body);
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
        curl_easy_setopt(m_curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms));
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
            if (m_added_to_multi) curl_multi_remove_handle(m_multi, m_curl);
            curl_easy_cleanup(m_curl);
        }
        if (m_header_list) curl_slist_free_all(m_header_list);
    }

    void run(CURLM* multi) {
        if (!m_curl) { finish("curl init failed"); return; }
        m_multi = multi;
        m_added_to_multi = true;
        curl_multi_add_handle(m_multi, m_curl);
    }

    void cancel() { m_cancelled.store(true); }
    bool is_cancelled() const { return m_cancelled.load(); }

    void on_transfer_done(const CURLcode code) {
        if (m_cancelled.load()) { finish(""); return; }
        if (code != CURLE_OK) { finish(curl_easy_strerror(code)); return; }

        // 检查 HTTP 状态码
        long http_code = 0;
        curl_easy_getinfo(m_curl, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code >= 400) {
            finish(std::string("HTTP error: ") + std::to_string(http_code));
            return;
        }
        if (m_reader && !m_reader->is_finished())
            m_reader->finish();
        if (m_on_complete) m_on_complete();
    }

private:
    void finish(const std::string& err) const {
        if (m_reader && !m_reader->is_finished())
            m_reader->finish(err);
        if (m_on_complete) m_on_complete();
    }

    static size_t stream_write_cb(void* ptr, size_t size, size_t nmemb, void* userdata) {
        auto* self = static_cast<StreamSession*>(userdata);
        if (self->m_cancelled.load()) return 0;
        if (self->m_reader)
            self->m_reader->feed_data(std::string(static_cast<const char*>(ptr), size * nmemb));
        return size * nmemb;
    }

    CURL* m_curl = nullptr;
    CURLM* m_multi = nullptr;
    bool m_added_to_multi = false;
    struct curl_slist* m_header_list = nullptr;
    std::string m_body;
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
                        // 也从 reader map 清理
                        for (auto rit = sessions_by_reader.begin(); rit != sessions_by_reader.end(); ) {
                            if (rit->second.expired()) rit = sessions_by_reader.erase(rit);
                            else ++rit;
                        }
                    }
                    if (session)
                        session->on_transfer_done(msg->data.result);
                }
            }

            if (still_running > 0)
                curl_multi_wait(multi, nullptr, 0, 100, nullptr);
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
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
    auto parsed = parse_url(url);
    auto* key = reader.get();

    const auto session = std::make_shared<StreamSession>(
        parsed, headers, body, reader, std::move(on_complete), timeout_ms);

    if (!session->easy_handle()) return;

    {
        std::lock_guard<std::mutex> lock(m_impl->sessions_mutex);
        m_impl->sessions_by_handle[session->easy_handle()] = session;
        m_impl->sessions_by_reader[key] = session;
    }

    session->run(m_impl->multi);
}

void HttpClient::cancel_stream(SSEStreamReader* reader) {
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

} // namespace workx