/**
 * @file sse_parser.hpp
 * @brief Server-Sent Events (SSE) + NDJSON 流式解析器
 * @details 增量解析，自动缓冲不完整数据块
 * @version 1.0.0
 */

#pragma once

#include <string>
#include <functional>
#include <sstream>
#include <map>

namespace agent {

struct SSEEvent {
    std::string data;
    std::string event;
    std::string id;
    int retry = 0;

    bool has_data() const { return !data.empty(); }
    bool has_event() const { return !event.empty(); }
    bool has_id() const { return !id.empty(); }
    bool has_retry() const { return retry > 0; }
};

class SSEParser {
public:
    using EventCallback = std::function<void(const SSEEvent&)>;

    explicit SSEParser(EventCallback callback);
    void parse(const std::string& chunk);
    void reset();

    const std::string& get_buffer() const { return m_buffer; }
    size_t get_event_count() const { return m_event_count; }

private:
    EventCallback m_callback;
    std::string m_buffer;
    size_t m_event_count = 0;

    void process_events();
    static SSEEvent parse_event(const std::string& event_text);
};

class NDJSONParser {
public:
    using LineCallback = std::function<void(const std::string&)>;

    explicit NDJSONParser(LineCallback callback);
    void parse(const std::string& chunk);
    void reset();

private:
    LineCallback m_callback;
    std::string m_buffer;

    void process_lines();
};

} // namespace agent
