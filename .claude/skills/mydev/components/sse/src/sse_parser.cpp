/**
 * @file sse_parser.cpp
 * @brief SSE + NDJSON 流式解析器实现
 */

#include "sse_parser.hpp"
#include <algorithm>

namespace mydev {

// ==================== SSEParser ====================

SSEParser::SSEParser(EventCallback callback)
    : m_callback(std::move(callback))
{
}

void SSEParser::parse(const std::string& chunk) {
    if (chunk.empty()) return;
    m_buffer += chunk;
    process_events();
}

void SSEParser::process_events() {
    if (m_buffer.empty()) return;

    size_t pos = 0;
    size_t last_event_end = 0;

    while (pos < m_buffer.length()) {
        size_t event_end = std::string::npos;

        // 查找 \n\n
        size_t nn_pos = m_buffer.find("\n\n", pos);
        if (nn_pos != std::string::npos) {
            event_end = nn_pos + 2;
        }

        // 查找 \r\n\r\n
        if (event_end == std::string::npos) {
            size_t rn_pos = m_buffer.find("\r\n\r\n", pos);
            if (rn_pos != std::string::npos) {
                event_end = rn_pos + 4;
            }
        }

        // 查找 \r\r
        if (event_end == std::string::npos) {
            size_t rr_pos = m_buffer.find("\r\r", pos);
            if (rr_pos != std::string::npos) {
                event_end = rr_pos + 2;
            }
        }

        if (event_end == std::string::npos) break;

        std::string event_text = m_buffer.substr(pos, event_end - pos - 2);

        if (!event_text.empty() && event_text.back() == '\r') {
            event_text.pop_back();
        }

        if (!event_text.empty()) {
            SSEEvent event = parse_event(event_text);
            if (event.has_data()) {
                m_callback(event);
                m_event_count++;
            }
        }

        last_event_end = event_end;
        pos = event_end;
    }

    if (last_event_end > 0) {
        m_buffer = m_buffer.substr(last_event_end);
    }
}

SSEEvent SSEParser::parse_event(const std::string& event_text) {
    SSEEvent event;

    std::istringstream stream(event_text);
    std::string line;

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.empty()) continue;

        size_t colon_pos = line.find(':');
        if (colon_pos == std::string::npos) continue;

        std::string field = line.substr(0, colon_pos);
        std::string value;

        if (colon_pos + 1 < line.length()) {
            if (line[colon_pos + 1] == ' ') {
                value = line.substr(colon_pos + 2);
            } else {
                value = line.substr(colon_pos + 1);
            }
        }

        if (field == "data") {
            if (event.has_data()) {
                event.data += "\n" + value;
            } else {
                event.data = value;
            }
        } else if (field == "event") {
            event.event = value;
        } else if (field == "id") {
            event.id = value;
        } else if (field == "retry") {
            try { event.retry = std::stoi(value); } catch (...) {}
        }
    }

    return event;
}

void SSEParser::reset() {
    m_buffer.clear();
    m_event_count = 0;
}

// ==================== NDJSONParser ====================

NDJSONParser::NDJSONParser(LineCallback callback)
    : m_callback(std::move(callback))
{
}

void NDJSONParser::parse(const std::string& chunk) {
    if (chunk.empty()) return;
    m_buffer += chunk;
    process_lines();
}

void NDJSONParser::process_lines() {
    if (m_buffer.empty()) return;

    size_t pos = 0;
    size_t last_line_end = 0;

    while (pos < m_buffer.length()) {
        size_t line_end = m_buffer.find('\n', pos);

        if (line_end == std::string::npos) break;

        std::string line = m_buffer.substr(pos, line_end - pos);

        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (!line.empty() && m_callback) {
            m_callback(line);
        }

        last_line_end = line_end + 1;
        pos = line_end + 1;
    }

    if (last_line_end > 0) {
        m_buffer = m_buffer.substr(last_line_end);
    }
}

void NDJSONParser::reset() {
    m_buffer.clear();
}

} // namespace mydev
