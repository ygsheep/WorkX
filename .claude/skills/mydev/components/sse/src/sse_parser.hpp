/**
 * @file sse_parser.hpp
 * @brief Server-Sent Events (SSE) + NDJSON 流式解析器
 * @details 增量解析，自动缓冲不完整数据块
 * @version 1.0.0
 *
 * 使用方式：复制 sse_parser.hpp + sse_parser.cpp 到项目
 */

#pragma once

#include <string>
#include <functional>
#include <sstream>
#include <map>

// ============================================================
// 命名空间：按需修改
// ============================================================
namespace mydev {

/**
 * @brief SSE 事件结构
 */
struct SSEEvent {
    std::string data;      ///< 事件数据（最常用）
    std::string event;     ///< 事件类型（可选）
    std::string id;        ///< 事件 ID（可选）
    int retry = 0;         ///< 重连间隔（可选，毫秒）

    bool has_data() const { return !data.empty(); }
    bool has_event() const { return !event.empty(); }
    bool has_id() const { return !id.empty(); }
    bool has_retry() const { return retry > 0; }
};

/**
 * @brief SSE 流式解析器
 *
 * 处理 SSE 格式的流式数据，支持：
 * - 缓冲不完整的数据块
 * - 按事件边界解析
 * - 自动处理各种换行格式
 */
class SSEParser {
public:
    using EventCallback = std::function<void(const SSEEvent&)>;

    /**
     * @brief 构造函数
     * @param callback 收到完整事件时的回调
     */
    explicit SSEParser(EventCallback callback);

    /**
     * @brief 解析数据块
     * @param chunk 收到的原始数据块（可能包含 0/1/多个完整事件 + 不完整尾部）
     */
    void parse(const std::string& chunk);

    /** @brief 重置解析器状态 */
    void reset();

    /** @brief 获取当前缓冲的数据 */
    const std::string& get_buffer() const { return m_buffer; }

    /** @brief 获取已解析的事件数量 */
    size_t get_event_count() const { return m_event_count; }

private:
    EventCallback m_callback;
    std::string m_buffer;
    size_t m_event_count = 0;

    void process_events();
    static SSEEvent parse_event(const std::string& event_text);
};

/**
 * @brief NDJSON 流式解析器（每行一个 JSON）
 */
class NDJSONParser {
public:
    using LineCallback = std::function<void(const std::string&)>;

    explicit NDJSONParser(LineCallback callback);

    /** @brief 解析数据块 */
    void parse(const std::string& chunk);

    /** @brief 重置解析器状态 */
    void reset();

private:
    LineCallback m_callback;
    std::string m_buffer;

    void process_lines();
};

} // namespace mydev
