/**
 * @file chat_renderer.cpp
 * @brief 聊天渲染器实现
 * @details 包含 TUI 状态机、思考视图切换、结构化输出
 * @version 2.0.0
 */

#include "tui/render/chat_renderer.h"
#include "tui/core/terminal.h"
#include "tui/core/color_scheme.h"
#include "tui/widgets/status_bar.h"
#include "tui/render/spinner.h"
#include "tui/render/output_formatter.h"
#include "tui/render/streaming_buffer.h"
#include "core/events/event_bus.h"
#include "agent/message/types.h"
#include <liblogger/logger.h>

#include <chrono>
#include <thread>
#include <algorithm>


namespace agent {

// ---- 详情视图按键码（与 line_editor.cpp 中一致，platform 层返回值）----
static constexpr char32_t KEY_ARROW_UP   = 0xE002;
static constexpr char32_t KEY_ARROW_DOWN = 0xE003;
static constexpr char32_t KEY_CTRL_O     = 0xE00A;

namespace {

// ---- 详情视图颜色常量（TrueColor RGB，对齐 Claude Code dark theme）----
constexpr const char* COLOR_THINKING = "\x1b[38;2;215;119;87m";  // Claude 橙（思考）
constexpr const char* COLOR_READ     = "\x1b[38;2;177;185;249m"; // 浅蓝紫（读取）
constexpr const char* COLOR_WRITE    = "\x1b[38;2;255;193;7m";   // 琥珀（写入）
constexpr const char* COLOR_FINAL    = "\x1b[38;2;78;186;101m";  // 亮绿（最终答复）
constexpr const char* COLOR_ERROR_C  = "\x1b[38;2;255;107;128m"; // 亮红（错误）
constexpr const char* COLOR_DIM_C    = "\x1b[38;2;153;153;153m"; // 浅灰 dimColor（标签/⎿ 前缀/行号）
constexpr const char* COLOR_REASON   = "\x1b[38;2;153;153;153m"; // 浅灰（推理内容）
constexpr const char* COLOR_RESET    = "\x1b[0m";

// diff 行背景色（暗色 TrueColor，对齐 Claude Code diffAdded/diffRemoved）
constexpr const char* BG_ADDED   = "\x1b[48;2;34;92;43m";    // 暗绿背景（新增行）
constexpr const char* BG_REMOVED = "\x1b[48;2;122;41;54m";   // 暗红背景（删除行）

// 按行分割字符串
std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> result;
    std::string current;
    for (char c : s) {
        if (c == '\n') {
            result.push_back(std::move(current));
            current.clear();
        } else {
            current += c;
        }
    }
    if (!current.empty()) {
        result.push_back(std::move(current));
    }
    if (result.empty()) result.push_back("");
    return result;
}

// 从工具参数 JSON 中提取 file_path 字段值（简单解析，避免依赖 nlohmann_json）
std::string extract_file_path(const std::string& args_json) {
    // 查找 "file_path":"..." 或 "path":"..."
    for (const char* key : {"\"file_path\"", "\"path\"", "\"file\""}) {
        auto pos = args_json.find(key);
        if (pos == std::string::npos) continue;
        pos = args_json.find(':', pos);
        if (pos == std::string::npos) continue;
        ++pos;
        // 跳过空白
        while (pos < args_json.size() && (args_json[pos] == ' ' || args_json[pos] == '\t')) ++pos;
        if (pos >= args_json.size() || args_json[pos] != '"') continue;
        ++pos;
        std::string val;
        while (pos < args_json.size() && args_json[pos] != '"') {
            if (args_json[pos] == '\\' && pos + 1 < args_json.size()) {
                ++pos;
            }
            val += args_json[pos++];
        }
        return val;
    }
    return "";
}

// 从工具参数 JSON 中提取字符串字段值（处理常见转义：\n \r \t \" \\ \/ \uXXXX）
// 返回是否成功找到字段；值通过 out 输出
bool extract_string_field(const std::string& args_json, const std::string& field_key,
                          std::string& out) {
    std::string needle = "\"" + field_key + "\"";
    auto pos = args_json.find(needle);
    if (pos == std::string::npos) return false;
    pos = args_json.find(':', pos + needle.size());
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < args_json.size() && (args_json[pos] == ' ' || args_json[pos] == '\t')) ++pos;
    if (pos >= args_json.size() || args_json[pos] != '"') return false;
    ++pos;

    out.clear();
    while (pos < args_json.size() && args_json[pos] != '"') {
        if (args_json[pos] == '\\' && pos + 1 < args_json.size()) {
            char esc = args_json[pos + 1];
            switch (esc) {
                case 'n':  out += '\n';      pos += 2; break;
                case 't':  out += '\t';      pos += 2; break;
                case 'r':  out += '\r';      pos += 2; break;
                case 'b':  out += '\b';      pos += 2; break;
                case 'f':  out += '\f';      pos += 2; break;
                case '"':  out += '"';       pos += 2; break;
                case '\\': out += '\\';      pos += 2; break;
                case '/':  out += '/';       pos += 2; break;
                case 'u': {
                    // \uXXXX：简单解码 BMP 平面，代理对按字面输出
                    if (pos + 5 < args_json.size()) {
                        std::string hex = args_json.substr(pos + 2, 4);
                        try {
                            unsigned int code = std::stoul(hex, nullptr, 16);
                            if (code < 0x80) {
                                out += static_cast<char>(code);
                            } else if (code < 0x800) {
                                out += static_cast<char>(0xC0 | (code >> 6));
                                out += static_cast<char>(0x80 | (code & 0x3F));
                            } else {
                                out += static_cast<char>(0xE0 | (code >> 12));
                                out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                                out += static_cast<char>(0x80 | (code & 0x3F));
                            }
                            pos += 6;
                        } catch (...) {
                            out += args_json[pos];
                            ++pos;
                        }
                    } else {
                        out += args_json[pos];
                        ++pos;
                    }
                    break;
                }
                default:
                    out += args_json[pos];
                    ++pos;
                    break;
            }
        } else {
            out += args_json[pos++];
        }
    }
    return true;
}

// 判断工具是否为写入类
bool is_write_tool(const std::string& name) {
    return name == "Write" || name == "write" || name == "FileWriteTool"
        || name == "Edit" || name == "edit" || name == "FileEditTool";
}

// 判断工具是否为读取类
bool is_read_tool(const std::string& name) {
    return name == "Read" || name == "read" || name == "FileReadTool";
}

// 获取工具的中文名称
std::string tool_cn_name(const std::string& name) {
    if (is_read_tool(name)) return "读取";
    if (name == "Edit" || name == "edit" || name == "FileEditTool") return "Edit";
    if (is_write_tool(name)) return "写入";
    return name;
}

// 渲染 Thought 条目到 lines（⏺ 标记 + 4 空格缩进，对齐 Claude Code transcript 风格）
void render_thought_entry(const LogEntry& entry, std::vector<std::string>& lines, bool is_final) {
    const char* color = is_final ? COLOR_FINAL : COLOR_THINKING;

    // 标题行：⏺ 思考 · 8s / ⏺ 最终答复
    if (is_final) {
        lines.push_back(std::format("  {}⏺ 最终答复{}", color, COLOR_RESET));
    } else {
        lines.push_back(std::format("  {}⏺ 思考 · {}s{}", color, entry.thinking_seconds, COLOR_RESET));
    }

    // 推理内容（单行内联，多行独立）
    if (!entry.reasoning.empty()) {
        auto reasoning_lines = split_lines(entry.reasoning);
        if (reasoning_lines.size() == 1) {
            lines.push_back(std::format("    {}推理：{}{}{}", COLOR_DIM_C, COLOR_REASON, reasoning_lines[0], COLOR_RESET));
        } else {
            lines.push_back(std::format("    {}推理：{}", COLOR_DIM_C, COLOR_RESET));
            for (const auto& l : reasoning_lines) {
                lines.push_back(std::format("    {}{}{}", COLOR_REASON, l, COLOR_RESET));
            }
        }
    }

    // 正文内容（单行内联，多行独立）
    if (!entry.content.empty()) {
        auto content_lines = split_lines(entry.content);
        if (content_lines.size() == 1) {
            lines.push_back(std::format("    {}正文：{}{}{}", COLOR_DIM_C, color, content_lines[0], COLOR_RESET));
        } else {
            lines.push_back(std::format("    {}正文：{}", COLOR_DIM_C, COLOR_RESET));
            for (const auto& l : content_lines) {
                lines.push_back(std::format("    {}{}{}", color, l, COLOR_RESET));
            }
        }
    } else if (!is_final) {
        lines.push_back(std::format("    {}正文：（无，已调用工具）{}", COLOR_DIM_C, COLOR_RESET));
    }
}

// 渲染 diff 行（标记 + 行号 + 双空格 + 代码，暗色 TrueColor 背景，对齐 Claude Code diffAdded/diffRemoved）
void render_diff_lines(const std::string& diff_text, std::vector<std::string>& lines) {
    auto diff_lines = split_lines(diff_text);

    // 第一遍：解析 hunk 头 + 内容行，记录最大行号用于对齐宽度
    struct ParsedLine {
        char prefix;        // '+', '-', ' '
        int line_num;
        std::string content;
    };
    std::vector<ParsedLine> parsed;
    int old_line = 0, new_line = 0;
    bool in_hunk = false;
    int max_line = 0;

    for (const auto& raw : diff_lines) {
        if (raw.empty()) continue;

        // 跳过 diff 头（不显示）
        if (raw.substr(0, 3) == "---") continue;
        if (raw.substr(0, 3) == "+++") continue;

        // 解析 hunk 头 @@ -old_start,old_len +new_start,new_len @@（不显示）
        if (raw.size() >= 2 && raw[0] == '@' && raw[1] == '@') {
            auto minus_pos = raw.find('-', 2);
            auto plus_pos = raw.find('+', minus_pos);
            if (minus_pos != std::string::npos && plus_pos != std::string::npos) {
                try {
                    old_line = std::stoi(raw.substr(minus_pos + 1));
                } catch (...) {}
                try {
                    new_line = std::stoi(raw.substr(plus_pos + 1));
                } catch (...) {}
            }
            in_hunk = true;
            continue;
        }

        if (!in_hunk) continue;

        char prefix = raw[0];
        std::string content = raw.substr(1);
        int line_num = 0;

        if (prefix == '+') {
            line_num = new_line++;
        } else if (prefix == '-') {
            line_num = old_line++;
        } else {
            line_num = new_line;
            ++old_line;
            ++new_line;
        }

        if (line_num > max_line) max_line = line_num;
        parsed.push_back({prefix, line_num, std::move(content)});
    }

    // 计算行号宽度（按最大行号位数）
    int width = 1;
    for (int n = max_line; n >= 10; n /= 10) ++width;

    // 第二遍：渲染
    for (const auto& p : parsed) {
        std::string num_str = std::to_string(p.line_num);
        if (static_cast<int>(num_str.size()) < width) {
            num_str = std::string(width - num_str.size(), ' ') + num_str;
        }

        if (p.prefix == '+') {
            // 新增：暗绿背景，前缀 +
            lines.push_back(std::format("      {}+{}  {}{}", BG_ADDED, num_str, p.content, COLOR_RESET));
        } else if (p.prefix == '-') {
            // 删除：暗红背景，前缀 -
            lines.push_back(std::format("      {}-{}  {}{}", BG_REMOVED, num_str, p.content, COLOR_RESET));
        } else {
            // 上下文：无背景，无前缀（与 +/- 的行号对齐：6 空格 + 1 空格占位 + 行号）
            lines.push_back(std::format("       {}  {}", num_str, p.content));
        }
    }
}

// 解析 Read 工具结果（FileReadTool 返回的 "N→内容" 格式），重新渲染为 Claude Code 风格的代码区
// 输出：⎿  Read N line(s) 标题行 + 每行 "       <num>  <content>"（7 空格缩进对齐 ⎿ 内容）
void render_read_content(const std::string& result, std::vector<std::string>& lines) {
    auto raw_lines = split_lines(result);

    // → (U+2192) UTF-8: \xe2\x86\x92
    const std::string arrow = "\xe2\x86\x92";

    struct ParsedReadLine {
        int line_num;
        std::string content;
        bool valid;
    };
    std::vector<ParsedReadLine> parsed;
    parsed.reserve(raw_lines.size());
    int max_line = 0;
    int valid_count = 0;

    for (const auto& raw : raw_lines) {
        auto arrow_pos = raw.find(arrow);
        if (arrow_pos == std::string::npos) {
            parsed.push_back({0, raw, false});
            continue;
        }

        std::string num_str = raw.substr(0, arrow_pos);
        size_t start = num_str.find_first_not_of(" \t");
        if (start == std::string::npos) {
            parsed.push_back({0, raw, false});
            continue;
        }
        num_str = num_str.substr(start);

        bool is_num = !num_str.empty();
        for (char c : num_str) {
            if (c < '0' || c > '9') {
                is_num = false;
                break;
            }
        }
        if (!is_num) {
            parsed.push_back({0, raw, false});
            continue;
        }

        int line_num = std::stoi(num_str);
        std::string content = raw.substr(arrow_pos + arrow.size());
        if (line_num > max_line) max_line = line_num;
        ++valid_count;
        parsed.push_back({line_num, std::move(content), true});
    }

    // 非代码格式结果（如目录列表、特殊提示）：原样显示
    if (valid_count == 0) {
        for (size_t i = 0; i < raw_lines.size(); ++i) {
            if (i == 0) {
                lines.push_back(std::format("    {}⎿  {}{}", COLOR_DIM_C, raw_lines[i], COLOR_RESET));
            } else {
                lines.push_back(std::format("       {}", raw_lines[i]));
            }
        }
        return;
    }

    // 计算行号宽度（按最大行号位数）
    int width = 1;
    for (int n = max_line; n >= 10; n /= 10) ++width;

    // 结果标题行：⎿  Read N line(s)
    std::string line_word = (valid_count == 1) ? "line" : "lines";
    lines.push_back(std::format("    {}⎿  Read {} {}{}",
        COLOR_DIM_C, valid_count, line_word, COLOR_RESET));

    // 代码区：每行 "       <dim><num><reset>  <content>"（无背景）
    for (const auto& p : parsed) {
        if (!p.valid) continue;  // 跳过空行、suffix 统计行
        std::string num_str = std::to_string(p.line_num);
        if (static_cast<int>(num_str.size()) < width) {
            num_str = std::string(width - num_str.size(), ' ') + num_str;
        }
        lines.push_back(std::format("       {}{}{}  {}",
            COLOR_DIM_C, num_str, COLOR_RESET, p.content));
    }
}

// 渲染工具调用条目（ToolCall + 已合并的 ToolResult）到 lines
// 对齐 Claude Code transcript：⏺ 标记 + 4 空格 ⎿ 结果前缀 + 7 空格代码区缩进
void render_tool_entry(const LogEntry& entry, std::vector<std::string>& lines) {
    const char* color = is_write_tool(entry.tool_name) ? COLOR_WRITE
                       : (is_read_tool(entry.tool_name) ? COLOR_READ : COLOR_THINKING);

    std::string file_path = extract_file_path(entry.arguments_json);
    std::string cn_name = tool_cn_name(entry.tool_name);

    // 标题行：⏺ 类型名  文件路径（类型与路径间双空格）
    if (file_path.empty()) {
        lines.push_back(std::format("  {}⏺ {}{}", color, cn_name, COLOR_RESET));
    } else {
        lines.push_back(std::format("  {}⏺ {}  {}{}", color, cn_name, file_path, COLOR_RESET));
    }

    // 结果区（带 ⎿ 前缀）
    if (entry.result.empty()) return;

    if (entry.is_error) {
        // 错误：亮红色 ⎿ 前缀 + 错误内容
        auto error_lines = split_lines(entry.result);
        for (size_t i = 0; i < error_lines.size(); ++i) {
            if (i == 0) {
                lines.push_back(std::format("    {}⎿  {}{}", COLOR_ERROR_C, error_lines[i], COLOR_RESET));
            } else {
                lines.push_back(std::format("       {}{}{}", COLOR_ERROR_C, error_lines[i], COLOR_RESET));
            }
        }
        return;
    }

    if (is_read_tool(entry.tool_name)) {
        // Read 工具：解析 N→内容 格式，渲染为 ⎿  Read N line(s) + 代码区
        render_read_content(entry.result, lines);
    } else if (is_write_tool(entry.tool_name)) {
        // Write 工具：检查是 diff（update 模式）还是 create 模式
        bool is_diff = (entry.result.size() >= 3 && entry.result.substr(0, 3) == "---");
        if (is_diff) {
            // update 模式：⎿  Updated <path> + diff 代码区
            lines.push_back(std::format("    {}⎿  Updated {}{}", COLOR_DIM_C, file_path, COLOR_RESET));
            render_diff_lines(entry.result, lines);
        } else {
            // create 模式：从 arguments_json 提取 content 渲染为带行号的代码区
            std::string file_content;
            bool has_content = extract_string_field(entry.arguments_json, "content", file_content)
                               && !file_content.empty();
            if (has_content) {
                auto content_lines = split_lines(file_content);
                size_t line_count = content_lines.size();
                std::string line_word = (line_count == 1) ? "line" : "lines";
                std::string title = file_path.empty()
                    ? std::format("Wrote {} {}", line_count, line_word)
                    : std::format("Wrote {} {} to {}", line_count, line_word, file_path);
                // 结果标题：⎿ 前缀（4 空格缩进对齐 ⏺ 之后内容）
                lines.push_back(std::format("    {}⎿  {}{}", COLOR_DIM_C, title, COLOR_RESET));

                // 代码区：每行 "  ┃<num>  <content>"（┃ 紧接行号，双空格分隔行号与内容）
                for (size_t i = 0; i < content_lines.size(); ++i) {
                    lines.push_back(std::format("  ┃{}{}{}  {}",
                        COLOR_DIM_C, std::to_string(i + 1), COLOR_RESET, content_lines[i]));
                }
            } else {
                // 回退：原样显示成功消息
                auto result_lines = split_lines(entry.result);
                for (size_t i = 0; i < result_lines.size(); ++i) {
                    if (i == 0) {
                        lines.push_back(std::format("    {}⎿  {}{}", COLOR_DIM_C, result_lines[i], COLOR_RESET));
                    } else {
                        lines.push_back(std::format("       {}", result_lines[i]));
                    }
                }
            }
        }
    } else {
        // 其他工具：默认 ⎿ 前缀 + 内容
        auto result_lines = split_lines(entry.result);
        for (size_t i = 0; i < result_lines.size(); ++i) {
            if (i == 0) {
                lines.push_back(std::format("    {}⎿  {}{}", COLOR_DIM_C, result_lines[i], COLOR_RESET));
            } else {
                lines.push_back(std::format("       {}{}{}", COLOR_DIM_C, result_lines[i], COLOR_RESET));
            }
        }
    }
}

} // anonymous namespace

ChatRenderer::ChatRenderer(Terminal* terminal)
    : m_terminal(terminal)
    , m_status_bar(std::make_unique<StatusBar>(terminal))
    , m_formatter(std::make_unique<OutputFormatter>(terminal))
    , m_stream_buf(std::make_unique<StreamingBuffer>(terminal))
{
}

ChatRenderer::~ChatRenderer() {
    stop();
}

void ChatRenderer::start() {
    auto& bus = EventBus::instance();

    // 启动会话计时
    m_status_bar->start_session_timer();
    m_status_bar->subscribe_events();

    // ---- BackendStatusEvent → 状态转换 ----
    m_token_status = std::make_unique<EventToken>(
        bus.subscribe<BackendStatusEvent>([this](const BackendStatusEvent& e) {
            if (e.status == BackendStatusEvent::Connecting) {
                if (m_state_machine.current() == TuiState::IDLE) {
                    m_streaming_started = false;
                    transition_to(TuiState::THINKING);
                    m_thinking_start_time = std::chrono::steady_clock::now();
                    m_thinking_seconds = 0;
                    m_reasoning_buffer.clear();

                    // 清空详情视图日志和当前 Thought 缓冲（新一轮 Agent 编排）
                    m_session_log.clear();
                    m_current_reasoning.clear();
                    m_current_content.clear();
                    m_current_step_number = 0;

                    // 启动 Spinner（思考计时）
                    m_terminal->spinner_start("Thinking");
                    m_spinner_active = true;

                    // 设置 Spinner 回调，只更新 StatusBar（差分渲染）
                    auto* spinner = m_terminal->get_spinner();
                    if (spinner) {
                        spinner->set_update_callback([this](int32_t seconds) {
                            m_thinking_seconds = seconds;
                            m_status_bar->set_thinking_seconds(seconds);
                            // 推进动画帧
                            m_status_bar->advance_frame();
                            // StatusBar 差分渲染
                            m_status_bar->render();
                        });
                    }

                    m_status_bar->set_state(TuiState::THINKING);
                    m_status_bar->render();
                }
            } else if (e.status == BackendStatusEvent::Connected) {
                if (m_spinner_active) {
                    m_terminal->spinner_stop();
                    m_spinner_active = false;
                }
            }
        })
    );

    // ---- StreamTokenEvent → 流式输出 ----
    m_token_stream = std::make_unique<EventToken>(
        bus.subscribe<StreamTokenEvent>([this](const StreamTokenEvent& e) {
            // 处理思考内容
            if (!e.reasoning_delta.empty()) {
                if (m_state_machine.current() != TuiState::THINKING) {
                    // 首次收到推理内容：输出 ● Thinking... 指示器
                    if (!m_detail_view_active) {
                        m_terminal->set_color(ColorRole::ThinkingIndicator);
                        m_terminal->write(" \xe2\x97\x8f Thinking... (ctrl+o to view)\n");
                        m_terminal->reset_color();
                    }

                    transition_to(TuiState::THINKING);
                    m_status_bar->set_state(TuiState::THINKING);
                    m_status_bar->render();
                }

                m_reasoning_buffer += e.reasoning_delta;
                m_current_reasoning += e.reasoning_delta;  // 累积到当前 Thought 缓冲

                if (m_viewing_thinking && !m_detail_view_active) {
                    m_terminal->set_color(ColorRole::Reasoning);
                    m_terminal->write(e.reasoning_delta);
                    m_terminal->reset_color();
                }
            }

            // 处理正文内容
            if (!e.content_delta.empty()) {
                m_current_content += e.content_delta;  // 累积到当前 Thought 缓冲

                if (!m_streaming_started.exchange(true)) {
                    // 第一次收到正文：切换到流式输出
                    if (!m_reasoning_buffer.empty() && !m_detail_view_active) {
                        m_terminal->set_color(ColorRole::Success);
                        m_terminal->write(" \xe2\x97\x8f Thought for ");
                        m_terminal->write(std::to_string(m_thinking_seconds));
                        m_terminal->write("s (ctrl+o to view)\n");
                        m_terminal->reset_color();
                    }

                    transition_to(TuiState::STREAMING);

                    m_stream_buf->start();
                    m_formatter->reset();
                }

                if (!m_viewing_thinking && !m_detail_view_active) {
                    m_formatter->feed(e.content_delta);
                }
            }

            // 更新 token 计数
            m_total_tokens += e.token_count;
            m_status_bar->set_token_count(m_total_tokens);
        })
    );

    // ---- StreamDoneEvent → 完成 ----
    m_token_done = std::make_unique<EventToken>(
        bus.subscribe<StreamDoneEvent>([this](const StreamDoneEvent& e) {
            if (m_spinner_active) {
                m_terminal->spinner_stop();
                m_spinner_active = false;
            }

            // 刷新 StreamingBuffer 和 OutputFormatter
            m_formatter->flush();
            m_stream_buf->stop();

            // 如果还在思考状态且有推理内容，输出 ● Thought for 标记
            if (m_state_machine.current() == TuiState::THINKING && !m_reasoning_buffer.empty() && !m_detail_view_active) {
                m_terminal->set_color(ColorRole::Success);
                m_terminal->write(" \xe2\x97\x8f Thought for ");  // ● (绿色)
                m_terminal->write(std::to_string(m_thinking_seconds));
                m_terminal->write("s (ctrl+o to view)\n");
                m_terminal->reset_color();
            }

            if (!m_detail_view_active) {
                m_terminal->write("\n");
            }

            // 显示 token 统计
            if (e.generated_tokens > 0 && !m_detail_view_active) {
                double tok_per_s = e.generation_ms > 0
                    ? (e.generated_tokens / (e.generation_ms / 1000.0))
                    : 0.0;
                m_terminal->set_color(ColorRole::TokenStats);
                char stats[128];
                snprintf(stats, sizeof(stats), "%.0f tokens \xe2\x8b\x85 %.1f tok/s \xe2\x8b\x85 %.1fs\n",
                    static_cast<double>(e.generated_tokens), tok_per_s,
                    e.generation_ms / 1000.0);
                m_terminal->write(stats);
                m_terminal->reset_color();
                m_total_tokens += e.generated_tokens;
            }

            m_message_count++;
            if (!m_detail_view_active) {
                m_terminal->mark_cursor_left_output();
                transition_to(TuiState::IDLE);
                m_status_bar->set_state(TuiState::IDLE);
                m_status_bar->set_token_count(m_total_tokens);
                m_status_bar->render();
                // 流结束，光标复位到输入行
                int h = m_terminal->get_terminal_height();
                int input_row = h - 1;
                if (input_row < 1) input_row = 1;
                m_terminal->cursor_to_pos(input_row, 3);
            }

            // 重置流式状态：为 ReAct 循环中下一轮 Thought 做准备
            // （中间 StreamDoneEvent 后，下一轮 Thought 需要重新初始化流式输出）
            m_streaming_started.store(false);
            m_reasoning_buffer.clear();
        })
    );

    // ---- StreamErrorEvent → 错误 ----
    m_token_error = std::make_unique<EventToken>(
        bus.subscribe<StreamErrorEvent>([this](const StreamErrorEvent& e) {
            if (m_spinner_active) {
                m_terminal->spinner_stop();
                m_spinner_active = false;
            }
            m_stream_buf->stop();
            m_formatter->reset();
            m_streaming_started.store(false);
            m_reasoning_buffer.clear();

            // 渲染错误块
            m_terminal->set_color(ColorRole::CodeBlock);
            m_terminal->write("\xe2\x94\x8c\xe2\x94\x80 Error ");  // ┌─ Error
            for (int i = 0; i < 40; ++i) m_terminal->write("\xe2\x94\x80");
            m_terminal->write("\xe2\x94\x90\n");  // ┐
            m_terminal->set_color(ColorRole::Error);
            m_terminal->write("\xe2\x94\x82 ");  // │
            m_terminal->write(e.message);
            m_terminal->write("\n");
            m_terminal->set_color(ColorRole::CodeBlock);
            m_terminal->write("\xe2\x94\x94");  // └
            for (int i = 0; i < 50; ++i) m_terminal->write("\xe2\x94\x80");
            m_terminal->write("\xe2\x94\x98\n");  // ┘
            m_terminal->reset_color();

            m_terminal->mark_cursor_left_output();
            // 将光标定位到输入行
            {
                int h = m_terminal->get_terminal_height();
                int input_row = h - 1;
                if (input_row < 1) input_row = 1;
                m_terminal->cursor_to_pos(input_row, 3);
            }
            transition_to(TuiState::ERROR);
            m_status_bar->set_state(TuiState::ERROR);
            m_status_bar->render();
        })
    );

    // ---- AgentStepEvent ----
    m_token_step = std::make_unique<EventToken>(
        bus.subscribe<AgentStepEvent>([this](const AgentStepEvent& e) {
            LOG_INFO("AgentStepEvent: step={} reasoning_len={} content_len={} log_size_before={}",
                e.step_number, m_current_reasoning.size(), m_current_content.size(), m_session_log.size());
            // 存入会话日志（当前 Thought 的 reasoning + content + seconds）
            m_session_log.add_thought(e.step_number, m_current_reasoning, m_current_content, m_thinking_seconds);
            m_current_step_number = e.step_number;
            LOG_INFO("AgentStepEvent: after add_thought, log_size={}, m_current_step_number={}",
                m_session_log.size(), m_current_step_number);
            // 清空当前 Thought 缓冲（为下一轮 Thought 准备）
            m_current_reasoning.clear();
            m_current_content.clear();

            // 渲染（详情视图激活时不写入终端，避免污染详情视图）
            if (!m_detail_view_active) {
                m_terminal->set_color(ColorRole::Bullet);
                m_terminal->write(std::format("  \xe2\x97\x8c Step {}: {}\n", e.step_number, e.description));
                m_terminal->reset_color();
            }
        })
    );

    // ---- ToolCallEvent ----
    m_token_tool_call = std::make_unique<EventToken>(
        bus.subscribe<ToolCallEvent>([this](const ToolCallEvent& e) {
            LOG_INFO("ToolCallEvent: tool={} step={} log_size_before={}",
                e.tool_name, m_current_step_number, m_session_log.size());
            // 存入会话日志
            m_session_log.add_tool_call(m_current_step_number, e.tool_name, e.arguments);
            // 保存最近一次工具调用（供 ToolResultEvent 渲染时区分工具类型）
            m_last_tool_name = e.tool_name;
            m_last_tool_arguments = e.arguments;
            LOG_INFO("ToolCallEvent: after add_tool_call, log_size={}", m_session_log.size());

            // 停止思考 spinner（工具执行阶段不再显示 "Thinking"）
            if (m_spinner_active) {
                m_terminal->spinner_stop();
                m_spinner_active = false;
            }

            // 渲染（详情视图激活时不写入终端）
            if (!m_detail_view_active) {
                auto icon = get_tool_icon(e.tool_name);
                std::string indent(m_tool_indent * 2, ' ');

                // 中文工具名 + 文件路径
                std::string cn_name = e.tool_name;
                if (e.tool_name == "Read" || e.tool_name == "read" || e.tool_name == "FileReadTool") cn_name = "读取";
                else if (e.tool_name == "Edit" || e.tool_name == "edit" || e.tool_name == "FileEditTool") cn_name = "Edit";
                else if (e.tool_name == "Write" || e.tool_name == "write" || e.tool_name == "FileWriteTool") cn_name = "写入";

                // 提取文件路径
                std::string file_path;
                for (const char* key : {"\"file_path\"", "\"path\"", "\"file\""}) {
                    auto pos = e.arguments.find(key);
                    if (pos == std::string::npos) continue;
                    pos = e.arguments.find(':', pos);
                    if (pos == std::string::npos) continue;
                    ++pos;
                    while (pos < e.arguments.size() && (e.arguments[pos] == ' ' || e.arguments[pos] == '\t')) ++pos;
                    if (pos >= e.arguments.size() || e.arguments[pos] != '"') continue;
                    ++pos;
                    while (pos < e.arguments.size() && e.arguments[pos] != '"') {
                        if (e.arguments[pos] == '\\' && pos + 1 < e.arguments.size()) ++pos;
                        file_path += e.arguments[pos++];
                    }
                    break;
                }

                m_terminal->set_color(ColorRole::ToolName);
                if (file_path.empty()) {
                    m_terminal->write(std::format("{}{} {} (ctrl+o to view)\n", indent, icon.icon, cn_name));
                } else {
                    m_terminal->write(std::format("{}{} {} {} (ctrl+o to view)\n", indent, icon.icon, cn_name, file_path));
                }
                m_terminal->reset_color();
            }

            // 更新 StatusBar
            m_status_bar->set_tool_name(e.tool_name);
            transition_to(TuiState::TOOL_RUNNING);
            m_status_bar->set_state(TuiState::TOOL_RUNNING);
            m_status_bar->render();

            m_tool_indent++;
        })
    );

    // ---- ToolResultEvent ----
    m_token_tool_result = std::make_unique<EventToken>(
        bus.subscribe<ToolResultEvent>([this](const ToolResultEvent& e) {
            LOG_INFO("ToolResultEvent: is_error={} result_len={} step={} log_size_before={}",
                e.is_error, e.result.size(), m_current_step_number, m_session_log.size());
            // 存入会话日志（合并到最近的 ToolCall entry）
            m_session_log.add_tool_result(m_current_step_number, e.result, e.is_error);
            LOG_INFO("ToolResultEvent: after add_tool_result, log_size={}", m_session_log.size());

            m_tool_indent = m_tool_indent > 0 ? m_tool_indent - 1 : 0;

            // 渲染（详情视图激活时不写入终端）
            if (!m_detail_view_active) {
                std::string indent(m_tool_indent * 2, ' ');

                // 成功/失败标记
                const char* marker = e.is_error
                    ? "\xe2\x9c\x97"  // ✗
                    : "\xe2\x9c\x93"; // ✓
                ColorRole marker_color = e.is_error ? ColorRole::Failure : ColorRole::Success;

                // Write create 模式：从 arguments 提取 content 渲染代码区
                // Write update/Edit 模式（result 含 diff）：走 else 分支用背景色渲染
                // 其他工具：按行显示 result（第一行加 ✓ ⎿ 前缀）
                bool is_diff_result = e.result.find("@@ -") != std::string::npos;
                bool is_write_create = is_write_tool(m_last_tool_name)
                    && !e.is_error
                    && !is_diff_result;

                if (is_write_create) {
                    std::string file_content;
                    if (extract_string_field(m_last_tool_arguments, "content", file_content)
                        && !file_content.empty()) {
                        auto content_lines = split_lines(file_content);
                        size_t line_count = content_lines.size();
                        std::string line_word = (line_count == 1) ? "line" : "lines";
                        std::string file_path = extract_file_path(m_last_tool_arguments);
                        std::string title = file_path.empty()
                            ? std::format("Wrote {} {}", line_count, line_word)
                            : std::format("Wrote {} {} to {}", line_count, line_word, file_path);

                        // 标题行：✓ ⎿  Wrote N lines to <path>
                        m_terminal->set_color(marker_color);
                        m_terminal->write(indent + "  ");
                        m_terminal->write(marker);
                        m_terminal->set_color(ColorRole::ToolOutput);
                        m_terminal->write(" \xe2\x8e\xbf  " + title + "\n");  // ⎿
                        m_terminal->reset_color();

                        // 空行分隔
                        m_terminal->write("\n");

                        // 代码区：┃(白色) <num>(灰色)  <content>(默认)
                        for (size_t i = 0; i < content_lines.size(); ++i) {
                            m_terminal->write(std::format("{}  \x1b[97m\xe2\x94\x83\x1b[90m{}  \x1b[0m{}\n",
                                indent, std::to_string(i + 1), content_lines[i]));  // ┃
                        }

                        // 空行分隔
                        m_terminal->write("\n");
                    } else {
                        // 回退：显示成功消息
                        m_terminal->set_color(marker_color);
                        m_terminal->write(indent + "  ");
                        m_terminal->write(marker);
                        m_terminal->set_color(ColorRole::ToolOutput);
                        m_terminal->write(" \xe2\x8e\xbf  " + e.result + "\n");  // ⎿
                        m_terminal->reset_color();
                    }
                } else {
                    // Read/Write update/Edit/其他：渲染标题行 + ┃ 代码区
                    auto result_lines = split_lines(e.result);
                    bool is_diff = is_diff_result;  // 复用前面的 diff 检测

                    // 标题行：Read N lines / Updated <path> / 原始首行
                    std::string title;
                    if (is_read_tool(m_last_tool_name)) {
                        const std::string arrow = "\xe2\x86\x92";  // →
                        int valid_count = 0;
                        for (const auto& line : result_lines) {
                            if (line.find(arrow) != std::string::npos) ++valid_count;
                        }
                        std::string line_word = (valid_count == 1) ? "line" : "lines";
                        title = std::format("Read {} {}", valid_count, line_word);
                    } else if (is_diff) {
                        std::string file_path = extract_file_path(m_last_tool_arguments);
                        title = file_path.empty() ? "Updated" : std::format("Updated {}", file_path);
                    } else {
                        title = result_lines.empty() ? "" : result_lines[0];
                    }

                    m_terminal->set_color(marker_color);
                    m_terminal->write(indent + "  ");
                    m_terminal->write(marker);
                    m_terminal->set_color(ColorRole::ToolOutput);
                    m_terminal->write(" \xe2\x8e\xbf  " + title + "\n");  // ⎿
                    m_terminal->reset_color();

                    // 空行分隔
                    m_terminal->write("\n");

                    // 代码区：┃<num>  <content>
                    const std::string arrow = "\xe2\x86\x92";  // →
                    if (is_read_tool(m_last_tool_name)) {
                        // Read：解析 N→content 格式，用原始行号
                        for (const auto& line : result_lines) {
                            auto arrow_pos = line.find(arrow);
                            if (arrow_pos == std::string::npos) continue;
                            std::string num_str = line.substr(0, arrow_pos);
                            size_t start = num_str.find_first_not_of(" \t");
                            if (start != std::string::npos) num_str = num_str.substr(start);
                            std::string content = line.substr(arrow_pos + arrow.size());
                            // ┃(白色) <num>(灰色)  <content>(默认)
                            m_terminal->write(std::format("{}  \x1b[97m\xe2\x94\x83\x1b[90m{}  \x1b[0m{}\n",
                                indent, num_str, content));  // ┃
                        }
                    } else if (is_diff) {
                        // Write update / Edit：解析 diff，去掉 +/- 标记，用背景色区分
                        int old_line = 0, new_line = 0;
                        bool in_hunk = false;
                        for (const auto& raw : result_lines) {
                            if (raw.empty()) continue;
                            if (raw.substr(0, 3) == "---" || raw.substr(0, 3) == "+++") continue;
                            if (raw.size() >= 2 && raw[0] == '@' && raw[1] == '@') {
                                auto minus_pos = raw.find('-', 2);
                                auto plus_pos = raw.find('+', minus_pos);
                                if (minus_pos != std::string::npos && plus_pos != std::string::npos) {
                                    try { old_line = std::stoi(raw.substr(minus_pos + 1)); } catch (...) {}
                                    try { new_line = std::stoi(raw.substr(plus_pos + 1)); } catch (...) {}
                                }
                                in_hunk = true;
                                continue;
                            }
                            if (!in_hunk) continue;

                            char prefix = raw[0];
                            std::string content = raw.substr(1);
                            std::string num_str;
                            const char* bg = nullptr;
                            if (prefix == '+') {
                                num_str = std::to_string(new_line++);
                                bg = BG_ADDED;
                            } else if (prefix == '-') {
                                num_str = std::to_string(old_line++);
                                bg = BG_REMOVED;
                            } else {
                                num_str = std::to_string(new_line);
                                ++old_line;
                                ++new_line;
                                bg = nullptr;
                            }
                            // ┃(白色) <num>(灰色)  <content>(默认)；有背景色时背景作用于整行
                            std::string line_out = indent + "  ";
                            if (bg) {
                                // 有背景色：┃+行号+内容 整体亮白前景 + 暗色背景
                                line_out += bg;
                                line_out += "\x1b[97m\xe2\x94\x83" + num_str + "  " + content;
                                line_out += "\x1b[0m";
                            } else {
                                // 无背景色（上下文行）：┃白 行号灰 内容默认
                                line_out += "\x1b[97m\xe2\x94\x83\x1b[90m" + num_str + "\x1b[0m  " + content;
                            }
                            line_out += "\n";
                            m_terminal->write(line_out);
                        }
                    } else {
                        // 其他工具：按行显示，行号从 1 开始递增
                        for (size_t i = 0; i < result_lines.size(); ++i) {
                            // ┃(白色) <num>(灰色)  <content>(默认)
                            m_terminal->write(std::format("{}  \x1b[97m\xe2\x94\x83\x1b[90m{}  \x1b[0m{}\n",
                                indent, std::to_string(i + 1), result_lines[i]));  // ┃
                        }
                    }

                    // 空行分隔
                    m_terminal->write("\n");
                }
            }

            transition_to(TuiState::STREAMING);
            m_status_bar->set_state(TuiState::STREAMING);
            m_status_bar->render();
        })
    );

    // ---- AgentDoneEvent ----
    m_token_agent_done = std::make_unique<EventToken>(
        bus.subscribe<AgentDoneEvent>([this](const AgentDoneEvent& e) {
            m_terminal->set_color(ColorRole::Success);
            m_terminal->write(std::format("  \xe2\x9c\x93 Agent done: {} steps, {} tool calls\n",
                e.total_steps, e.total_tool_calls));
            m_terminal->reset_color();
            m_tool_indent = 0;

            // 光标复位到输入行（AgentDoneEvent 在 StreamDoneEvent 之后发布，
            // 前者的 write() 会把光标重新拉回输出区，因此这里需要再次复位）
            m_terminal->mark_cursor_left_output();
            int h = m_terminal->get_terminal_height();
            int input_row = h - 1;
            if (input_row < 1) input_row = 1;
            m_terminal->cursor_to_pos(input_row, 3);
        })
    );
}

void ChatRenderer::stop() {
    auto& bus = EventBus::instance();

    if (m_spinner_active) {
        m_terminal->spinner_stop();
        m_spinner_active = false;
    }
    m_stream_buf->stop();
    m_status_bar->unsubscribe_events();

    if (m_token_status && m_token_status->is_valid()) {
        bus.unsubscribe<BackendStatusEvent>(*m_token_status);
    }
    if (m_token_stream && m_token_stream->is_valid()) {
        bus.unsubscribe<StreamTokenEvent>(*m_token_stream);
    }
    if (m_token_done && m_token_done->is_valid()) {
        bus.unsubscribe<StreamDoneEvent>(*m_token_done);
    }
    if (m_token_error && m_token_error->is_valid()) {
        bus.unsubscribe<StreamErrorEvent>(*m_token_error);
    }
    if (m_token_step && m_token_step->is_valid()) {
        bus.unsubscribe<AgentStepEvent>(*m_token_step);
    }
    if (m_token_tool_call && m_token_tool_call->is_valid()) {
        bus.unsubscribe<ToolCallEvent>(*m_token_tool_call);
    }
    if (m_token_tool_result && m_token_tool_result->is_valid()) {
        bus.unsubscribe<ToolResultEvent>(*m_token_tool_result);
    }
    if (m_token_agent_done && m_token_agent_done->is_valid()) {
        bus.unsubscribe<AgentDoneEvent>(*m_token_agent_done);
    }
}

void ChatRenderer::transition_to(TuiState new_state) {
    m_state_machine.transition_to(new_state);
    if (m_state_machine.current() != new_state) {
        m_state_machine.force_state(new_state);
    }
}

void ChatRenderer::toggle_thinking_view() {
    toggle_detail_view();
}

void ChatRenderer::toggle_detail_view() {
    if (m_detail_view_active) {
        // 退出详情视图
        LOG_INFO("toggle_detail_view: EXIT, log_size={}", m_session_log.size());
        collapse_detail_view();
    } else {
        // 进入详情视图
        LOG_INFO("toggle_detail_view: ENTER, log_size={}", m_session_log.size());
        if (m_session_log.empty()) return;  // 无内容，不进入

        // 保存终端快照（覆盖层机制），退出时恢复，避免清屏丢失对话历史
        int h = m_terminal->get_terminal_height();
        int scroll_bottom = h - 3;
        if (scroll_bottom < 1) scroll_bottom = 1;
        m_terminal->begin_overlay(1, scroll_bottom);

        m_detail_view_active = true;
        m_scroll_offset = 0;
        m_state_machine.force_state(TuiState::DETAIL_VIEW);
        render_detail_view();
    }
}

void ChatRenderer::collapse_detail_view() {
    m_detail_view_active = false;
    m_scroll_offset = 0;
    m_state_machine.force_state(TuiState::IDLE);

    // 恢复进入详情视图前保存的终端快照（包含完整对话历史）
    m_terminal->end_overlay();

    // 恢复滚动区域（详情视图期间被 reset 了）
    m_terminal->setup_scroll_region();

    // 重渲状态栏（快照中不含状态栏）
    m_status_bar->render();

    // 光标复位到输入行
    m_terminal->mark_cursor_left_output();
    int h = m_terminal->get_terminal_height();
    int input_row = h - 1;
    if (input_row < 1) input_row = 1;
    m_terminal->cursor_to_pos(input_row, 3);
}

void ChatRenderer::render_detail_view() {
    // 1. 生成所有渲染行
    std::vector<std::string> lines;

    {
        std::string entry_types;
        for (const auto& entry : m_session_log.entries()) {
            const char* tn = "Unknown";
            switch (entry.type) {
                case LogEntryType::Thought: tn = "Thought"; break;
                case LogEntryType::FinalAnswer: tn = "FinalAnswer"; break;
                case LogEntryType::ToolCall: tn = "ToolCall"; break;
                case LogEntryType::ToolResult: tn = "ToolResult"; break;
            }
            if (!entry_types.empty()) entry_types += ", ";
            entry_types += std::format("{}(step={}", tn, entry.step_number);
            if (entry.type == LogEntryType::ToolCall) {
                entry_types += std::format(", tool={}, result_len={}", entry.tool_name, entry.result.size());
            }
            entry_types += ")";
        }
        LOG_INFO("render_detail_view: entries=[{}]", entry_types);
    }

    for (const auto& entry : m_session_log.entries()) {
        switch (entry.type) {
            case LogEntryType::Thought:
                render_thought_entry(entry, lines, false);
                break;
            case LogEntryType::FinalAnswer:
                render_thought_entry(entry, lines, true);
                break;
            case LogEntryType::ToolCall:
                render_tool_entry(entry, lines);
                break;
            case LogEntryType::ToolResult:
                // 独立的 ToolResult（无对应 ToolCall）：⎿ 前缀简单渲染
                lines.push_back(std::format("    {}⎿  {}{}",
                    entry.is_error ? COLOR_ERROR_C : COLOR_DIM_C,
                    entry.result, COLOR_RESET));
                break;
        }
        lines.push_back("");  // 卡片之间空一行
    }

    int total_lines = static_cast<int>(lines.size());

    // 2. 重置滚动区域（避免 write() 把光标强制归位到 scroll_bottom）
    m_terminal->reset_scroll_region();

    // 3. 清屏
    m_terminal->write("\x1b[2J\x1b[H");

    // 3. 计算视口高度（顶部标题 1 + 分隔线 1 + 底部分隔线 1 + 状态行 1 = 4）
    int term_h = m_terminal->get_terminal_height();
    int term_w = m_terminal->get_terminal_width();
    int viewport_h = term_h - 4;
    if (viewport_h < 1) viewport_h = 1;

    // 4. 调整 scroll_offset
    int max_offset = total_lines > viewport_h ? total_lines - viewport_h : 0;
    if (m_scroll_offset < 0) m_scroll_offset = 0;
    if (m_scroll_offset > max_offset) m_scroll_offset = max_offset;

    // 5. 渲染顶部标题行（对齐 cc.md：去掉"共 N 步"，保留 Ctrl+O 返回提示）
    m_terminal->set_color(ColorRole::System);
    m_terminal->write("详情视图 · Ctrl+O 返回\n");
    m_terminal->reset_color();

    // 分隔线
    m_terminal->set_color(ColorRole::Dim);
    for (int i = 0; i < term_w; ++i) m_terminal->write("\xe2\x94\x80");  // ─
    m_terminal->write("\n");
    m_terminal->reset_color();

    // 6. 渲染可见行
    int end = std::min(m_scroll_offset + viewport_h, total_lines);
    for (int i = m_scroll_offset; i < end; ++i) {
        m_terminal->write(lines[i]);
        m_terminal->write("\n");
    }

    // 7. 渲染底部分隔线
    m_terminal->set_color(ColorRole::Dim);
    for (int i = 0; i < term_w; ++i) m_terminal->write("\xe2\x94\x80");
    m_terminal->write("\n");

    // 状态行
    int percent = total_lines > 0 ? (m_scroll_offset * 100 / total_lines) : 0;
    m_terminal->write(std::format("\xe2\x86\x91\xe2\x86\x93 j\xc2\xb7k \xe6\xbb\x9a\xe5\x8a\xa8 \xc2\xb7 g/G \xe9\xa1\xb6\xe5\xba\x95 \xc2\xb7 q \xe6\x88\x96 Ctrl+O \xe8\xbf\x94\xe5\x9b\x9e \xc2\xb7 {}%\n", percent));
    m_terminal->reset_color();
}

bool ChatRenderer::handle_detail_input(char32_t key) {
    if (!m_detail_view_active) return false;

    // 退出键：q / Q / Esc / Ctrl+O
    if (key == 'q' || key == 'Q' || key == KEY_CTRL_O || key == 0x1B) {
        collapse_detail_view();
        return false;
    }

    // 计算总行数和视口高度
    int total_lines = static_cast<int>(m_session_log.size()) * 4;  // 粗略估计
    int term_h = m_terminal->get_terminal_height();
    int viewport_h = term_h - 4;
    if (viewport_h < 1) viewport_h = 1;

    // 重新计算总行数（精确）
    {
        std::vector<std::string> lines;
        for (const auto& entry : m_session_log.entries()) {
            switch (entry.type) {
                case LogEntryType::Thought:
                    render_thought_entry(entry, lines, false);
                    break;
                case LogEntryType::FinalAnswer:
                    render_thought_entry(entry, lines, true);
                    break;
                case LogEntryType::ToolCall:
                    render_tool_entry(entry, lines);
                    break;
                case LogEntryType::ToolResult:
                    lines.push_back(std::format("    {}⎿  {}{}",
                        entry.is_error ? COLOR_ERROR_C : COLOR_DIM_C,
                        entry.result, COLOR_RESET));
                    break;
            }
            lines.push_back("");
        }
        total_lines = static_cast<int>(lines.size());
    }

    int max_offset = total_lines > viewport_h ? total_lines - viewport_h : 0;

    // 滚动键
    if (key == 'j' || key == KEY_ARROW_DOWN) {
        if (m_scroll_offset < max_offset) {
            ++m_scroll_offset;
            render_detail_view();
        }
    } else if (key == 'k' || key == KEY_ARROW_UP) {
        if (m_scroll_offset > 0) {
            --m_scroll_offset;
            render_detail_view();
        }
    } else if (key == 'g') {
        if (m_scroll_offset != 0) {
            m_scroll_offset = 0;
            render_detail_view();
        }
    } else if (key == 'G') {
        if (m_scroll_offset != max_offset) {
            m_scroll_offset = max_offset;
            render_detail_view();
        }
    } else if (key == ' ') {
        // 空格：向下翻页
        m_scroll_offset += viewport_h;
        if (m_scroll_offset > max_offset) m_scroll_offset = max_offset;
        render_detail_view();
    } else if (key == 'b') {
        // b：向上翻页
        m_scroll_offset -= viewport_h;
        if (m_scroll_offset < 0) m_scroll_offset = 0;
        render_detail_view();
    }
    // 其他键：吞掉，不处理

    return true;  // 仍在详情视图
}

} // namespace agent
