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
#include "tui/render/markdown_renderer.h"
#include "tui/render/streaming_buffer.h"
#include "tui/render/syntax_highlighter.h"
#include "core/events/event_bus.h"
#include "agent/message/types.h"

#include <cassert>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <nlohmann/json.hpp>

namespace tui {

using namespace agent;  // P0: tui→agent 类型引用过渡方案，后续 P2/P3 收紧到显式前缀

namespace {

/// @brief 从文件路径扩展名推断 tree-sitter grammar 语言标签
/// @return 语言字符串 (cpp/python/...); 未知扩展返回空
std::string lang_from_path(const std::string& path) {
    std::filesystem::path p(path);
    std::string ext = p.extension().string();
    if (ext.empty()) return {};
    // 去掉前导 '.', 转小写
    if (ext[0] == '.') ext.erase(0, 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    static const std::unordered_map<std::string, std::string> mapping = {
        {"c", "c"}, {"h", "c"},
        {"cc", "cpp"}, {"cxx", "cpp"}, {"cpp", "cpp"}, {"hpp", "cpp"}, {"hxx", "cpp"}, {"ino", "cpp"},
        {"py", "python"}, {"pyw", "python"}, {"pyi", "python"},
        {"js", "javascript"}, {"jsx", "javascript"}, {"mjs", "javascript"}, {"cjs", "javascript"},
        {"ts", "typescript"}, {"tsx", "typescript"},
        {"rs", "rust"},
        {"go", "go"},
        {"sh", "bash"}, {"bash", "bash"}, {"zsh", "bash"}, {"ksh", "bash"},
        {"json", "json"},
        {"cmake", "cmake"}, {"txt", "cmake"},  // CMakeLists.txt 没扩展名, 走兜底
    };
    auto it = mapping.find(ext);
    if (it != mapping.end()) return it->second;
    // CMakeLists.txt / *.cmake.in
    std::string name = p.filename().string();
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (name == "cmakelists.txt") return "cmake";
    return {};
}

/// @brief 从 ToolCallEvent.arguments (JSON 字符串) 解析 file_path
/// @return file_path 字符串; 解析失败或字段不存在返回空
std::string extract_file_path(const std::string& arguments_json) {
    if (arguments_json.empty()) return {};
    try {
        auto j = nlohmann::json::parse(arguments_json);
        if (j.contains("file_path") && j["file_path"].is_string()) {
            return j["file_path"].get<std::string>();
        }
    } catch (...) {
        // 解析失败忽略, 走默认渲染
    }
    return {};
}

/// @brief 判断是否为代码类工具 (返回内容应走语法高亮)
bool is_code_tool(const std::string& tool_name) {
    return tool_name == "Read" || tool_name == "Write" || tool_name == "Edit"
        || tool_name == "FileRead" || tool_name == "FileWrite" || tool_name == "FileEdit";
}

/// @brief 把 FileRead 工具的带行号输出 (如 "  123→code") 拆分为 (行号前缀, 代码内容)
/// @param line  单行文本
/// @param[out] line_prefix  行号前缀 (含 → 符号), 如 "  123→"; 无行号则为空
/// @param[out] code_part    代码部分 (行号之后的内容)
void split_fileread_line(const std::string& line, std::string& line_prefix, std::string& code_part) {
    line_prefix.clear();
    code_part = line;
    // 跳过前导空白
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    size_t num_start = i;
    while (i < line.size() && line[i] >= '0' && line[i] <= '9') ++i;
    if (i == num_start) return;  // 没有数字, 不是行号格式
    // 紧跟着 → (U+2192, UTF-8: E2 86 92)
    if (i + 3 > line.size()) return;
    if (static_cast<unsigned char>(line[i]) != 0xE2
        || static_cast<unsigned char>(line[i + 1]) != 0x86
        || static_cast<unsigned char>(line[i + 2]) != 0x92) return;
    size_t arrow_end = i + 3;
    line_prefix = line.substr(0, arrow_end);
    code_part = line.substr(arrow_end);
}

/// @brief 判断一行是否是 FileRead 末尾的元数据行
/// @details 匹配 file_read_tool.cpp 末尾追加的格式:
///          "(N of M lines shown)" / "(N lines shown, more available)"
bool is_fileread_metadata_line(const std::string& line) {
    // 跳过前导空白
    size_t i = 0;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    if (i >= line.size() || line[i] != '(') return false;
    ++i;
    // 至少一个数字
    size_t num_start = i;
    while (i < line.size() && line[i] >= '0' && line[i] <= '9') ++i;
    if (i == num_start) return false;
    // 紧跟着 " of " 或 " lines shown"
    if (i + 4 <= line.size() && line.compare(i, 4, " of ") == 0) return true;
    if (i + 12 <= line.size() && line.compare(i, 12, " lines shown") == 0) return true;
    return false;
}

/// @brief 渲染代码类工具的结果 (FileRead/Write/Edit)
/// @details
///   - FileRead: 剥离行号 → 整体高亮代码 → 加回行号 (行号用 Dim 色)
///               末尾元数据行 "(N of M lines shown)" 不高亮, 以 Dim 色输出
///   - Write/Edit: 结果格式为 "状态文本\n\n<diff>", 拆分:
///       状态行原样输出 (Dim 色), diff 部分走 highlight_code("diff", ...)
///   - 用 indent 缩进 + ⎿ 符号包裹, 限制最大行数避免刷屏
std::string render_code_tool_result(
    const std::string& tool_name,
    const std::string& arguments_json,
    const std::string& result,
    const std::string& indent,
    bool is_error)
{
    const std::string file_path = extract_file_path(arguments_json);

    // 错误结果不高亮, 走原样输出
    if (is_error) {
        std::string out = indent + "  \xe2\x9c\x97 \xe2\x8e\xbf  " + result + "\n";
        return out;
    }

    constexpr std::string_view dim = "\x1b[2m";
    constexpr std::string_view reset = "\x1b[0m";
    constexpr int MAX_DISPLAY_LINES = 60;

    std::ostringstream os;
    const std::string arrow = "\xe2\x8e\xbf";  // ⎿

    const bool is_fileread = (tool_name == "Read" || tool_name == "FileRead");
    const bool is_write_or_edit = (tool_name == "Write" || tool_name == "Edit"
                                   || tool_name == "FileWrite" || tool_name == "FileEdit");

    // ============================================================
    // 分支 A: Write/Edit — 拆分状态文本 + diff
    // ============================================================
    if (is_write_or_edit) {
        // 找到 diff 起始行 (以 "--- " 开头)
        // 之前的行是状态文本, 之后的行是 diff 内容
        size_t pos = 0;
        size_t diff_start = std::string::npos;
        size_t line_start = 0;
        while (pos <= result.size()) {
            if (pos == result.size() || result[pos] == '\n') {
                // 检查当前行 (line_start .. pos) 是否以 "--- " 开头
                if (pos - line_start >= 4 && result.compare(line_start, 4, "--- ") == 0) {
                    diff_start = line_start;
                    break;
                }
                line_start = pos + 1;
            }
            ++pos;
        }

        if (diff_start == std::string::npos) {
            // 没有 diff (create 模式 / no changes / 其他) — 退回 preview 路径
            return std::string{};
        }

        // 输出状态文本部分 (diff 之前的内容), 去掉末尾空行
        std::string status_text = result.substr(0, diff_start);
        while (!status_text.empty() && status_text.back() == '\n') {
            status_text.pop_back();
        }
        if (!status_text.empty()) {
            // 按 \n split 逐行输出
            size_t s = 0;
            while (s <= status_text.size()) {
                size_t nl = status_text.find('\n', s);
                std::string line = (nl == std::string::npos)
                    ? status_text.substr(s)
                    : status_text.substr(s, nl - s);
                os << indent << "  " << arrow << " " << dim << line << reset << "\n";
                if (nl == std::string::npos) break;
                s = nl + 1;
            }
        }

        // diff 部分走 highlight_diff (前景色用文件语言高亮, 背景色按 +/- 着色)
        std::string diff_text = result.substr(diff_start);
        std::string highlighted = highlight_diff(lang_from_path(file_path), diff_text);

        // 按 \n split 高亮后的 diff
        std::vector<std::string> diff_lines;
        {
            std::string cur;
            for (char c : highlighted) {
                if (c == '\n') { diff_lines.push_back(std::move(cur)); cur.clear(); }
                else cur.push_back(c);
            }
            if (!cur.empty()) diff_lines.push_back(std::move(cur));
        }

        // 限制最大显示行数
        const bool truncated = static_cast<int>(diff_lines.size()) > MAX_DISPLAY_LINES;
        if (truncated) diff_lines.resize(MAX_DISPLAY_LINES);

        for (const auto& l : diff_lines) {
            os << indent << "  " << arrow << "  " << l;
            if (!l.empty()) os << reset;
            os << "\n";
        }
        if (truncated) {
            os << indent << "  " << arrow << " " << dim
               << "(... truncated, showing first " << MAX_DISPLAY_LINES << " lines)"
               << reset << "\n";
        }
        return os.str();
    }

    // ============================================================
    // 分支 B: FileRead — 剥离行号 + 末尾元数据
    // ============================================================
    if (!is_fileread) {
        // 其他代码工具 (兜底) — 走 preview 路径
        return std::string{};
    }

    // 按行 split
    std::vector<std::string> lines;
    {
        std::string cur;
        for (char c : result) {
            if (c == '\n') { lines.push_back(std::move(cur)); cur.clear(); }
            else cur.push_back(c);
        }
        lines.push_back(std::move(cur));
    }

    // 剥离末尾元数据行: 从末尾向前扫描, 收集连续的元数据行 + 它们之前的空行
    std::vector<std::string> meta_lines;
    while (!lines.empty()) {
        const std::string& last = lines.back();
        if (last.empty()) {
            meta_lines.insert(meta_lines.begin(), last);
            lines.pop_back();
        } else if (is_fileread_metadata_line(last)) {
            meta_lines.insert(meta_lines.begin(), last);
            lines.pop_back();
        } else {
            break;
        }
    }

    // 限制最大显示行数 (代码部分, 不含元数据)
    const bool truncated = static_cast<int>(lines.size()) > MAX_DISPLAY_LINES;
    if (truncated) lines.resize(MAX_DISPLAY_LINES);

    // 剥离行号前缀, 拼成纯代码块
    std::vector<std::string> line_prefixes;
    line_prefixes.reserve(lines.size());
    std::string code_blob;
    for (const auto& l : lines) {
        std::string prefix, code;
        split_fileread_line(l, prefix, code);
        line_prefixes.push_back(prefix);
        if (!code_blob.empty()) code_blob.push_back('\n');
        code_blob += code;
    }

    // 整体高亮 (tree-sitter 需要看完整代码才能正确解析)
    const std::string lang = lang_from_path(file_path);
    std::string highlighted = lang.empty() ? code_blob : highlight_code(lang, code_blob);

    // 按行重新拆开
    std::vector<std::string> hl_lines;
    {
        std::string cur;
        for (char c : highlighted) {
            if (c == '\n') { hl_lines.push_back(std::move(cur)); cur.clear(); }
            else cur.push_back(c);
        }
        hl_lines.push_back(std::move(cur));
        if (hl_lines.size() != lines.size()) {
            // 行数不一致 (不应发生), 退回未高亮版本
            hl_lines = lines;
        }
    }

    // 组装代码行: indent + "  ⎿ " + [行号前缀(Dim)] + 高亮代码行 + RESET + "\n"
    for (size_t i = 0; i < hl_lines.size(); ++i) {
        os << indent << "  " << arrow;
        if (i < line_prefixes.size() && !line_prefixes[i].empty()) {
            os << " " << dim << line_prefixes[i] << reset;
            os << hl_lines[i];
            os << reset;
        } else {
            os << "  " << hl_lines[i];
            if (!hl_lines[i].empty()) os << reset;
        }
        os << "\n";
    }

    if (truncated) {
        os << indent << "  " << arrow << " " << dim
           << "(... truncated, showing first " << MAX_DISPLAY_LINES << " lines)"
           << reset << "\n";
    }

    // 追加元数据行 (Dim 色, 不高亮)
    for (const auto& m : meta_lines) {
        if (m.empty()) {
            os << "\n";
        } else {
            os << indent << "  " << arrow << " " << dim << m << reset << "\n";
        }
    }

    return os.str();
}

} // namespace

ChatRenderer::ChatRenderer(Terminal* terminal)
    : m_terminal(terminal)
    , m_status_bar(std::make_unique<StatusBar>(terminal))
    , m_formatter(std::make_unique<OutputFormatter>(terminal))
    , m_stream_buf(std::make_unique<StreamingBuffer>(terminal))
{
    // L-3 生命周期契约：Terminal 必须长于 ChatRenderer，由调用方（main.cpp）保证
    assert(terminal != nullptr && "ChatRenderer: Terminal must outlive ChatRenderer");
}

ChatRenderer::~ChatRenderer() {
    stop();
}

void ChatRenderer::start() {
    // D-4：复用 Terminal 的 DI 路径，消除对 EventBus 单例的直接依赖
    auto& bus = m_terminal->event_bus();

    // 启动会话计时
    m_status_bar->start_session_timer();
    m_status_bar->subscribe_events();

    // ---- BackendStatusEvent → 状态转换 ----
    m_token_status = std::make_unique<EventToken>(
        bus.subscribe<BackendStatusEvent>([this](const BackendStatusEvent& e) {
            if (e.status == BackendStatusEvent::Connecting) {
                if (m_state_machine.current() == TuiState::IDLE) {
                    m_streaming_started.store(false);
                    transition_to(TuiState::THINKING);
                    m_thinking_start_time = std::chrono::steady_clock::now();
                    m_thinking_seconds.store(0);
                    m_reasoning_buffer.clear();

                    // 启动 Spinner（思考计时）
                    m_terminal->spinner_start("Thinking");
                    m_spinner_active.store(true);

                    // 设置 Spinner 回调，只更新 StatusBar（差分渲染）
                    auto* spinner = m_terminal->get_spinner();
                    if (spinner) {
                        spinner->set_update_callback([this](int32_t seconds) {
                            m_thinking_seconds.store(seconds);
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
                if (m_spinner_active.load()) {
                    m_terminal->spinner_stop();
                    m_spinner_active.store(false);
                }
            }
        })
    );

    // ---- UserInputEvent → 本地估算用户输入 token ----
    // 当 provider 不返回 usage 时，用于累加用户输入 token 到上下文估算
    // 注意：此处只更新数据，不调用 render()。StatusBar 在用户输入行下方，
    // 立即 render 会把光标拉到 status_row 导致光标错位。
    // StreamDoneEvent / AgentDoneEvent 后会自然触发 render 刷新显示。
    m_token_user_input = std::make_unique<EventToken>(
        bus.subscribe<UserInputEvent>([this](const UserInputEvent& e) {
            m_token_stats.add_user_input(e.text);
            m_status_bar->set_token_count(m_token_stats.total_tokens());
        })
    );

    // ---- StreamTokenEvent → 流式输出 ----
    m_token_stream = std::make_unique<EventToken>(
        bus.subscribe<StreamTokenEvent>([this](const StreamTokenEvent& e) {
            // 处理思考内容
            if (!e.reasoning_delta.empty()) {
                if (m_state_machine.current() != TuiState::THINKING) {
                    // 首次收到推理内容：输出 ● 思考中... 指示器
                    m_terminal->set_color(ColorRole::ThinkingIndicator);
                    m_terminal->write(" \xe2\x97\x8f \xe6\x80\x9d\xe8\x80\x83\xe4\xb8\xad... (ctrl+o \xe6\x9f\xa5\xe7\x9c\x8b)\n");
                    m_terminal->reset_color();

                    transition_to(TuiState::THINKING);
                    m_status_bar->set_state(TuiState::THINKING);
                    m_status_bar->render();
                }

                m_reasoning_buffer += e.reasoning_delta;

                // H-1 修复：overlay 期间不写入终端（避免破坏思考视图显示）
                // 思考内容已追加到 m_reasoning_buffer，用户下次展开时可看到完整内容
                if (m_viewing_thinking.load()) {
                    m_terminal->set_color(ColorRole::Reasoning);
                    m_terminal->write(e.reasoning_delta);
                    m_terminal->reset_color();
                }
            }

            // 处理正文内容
            if (!e.content_delta.empty()) {
                if (!m_streaming_started.exchange(true)) {
                    // 第一次收到正文：切换到流式输出
                    if (!m_reasoning_buffer.empty()) {
                        m_terminal->set_color(ColorRole::Success);
                        m_terminal->write(" \xe2\x97\x8f \xe6\x80\x9d\xe8\x80\x83 ");
                        m_terminal->write(std::to_string(m_thinking_seconds.load()));
                        m_terminal->write("s (ctrl+o \xe6\x9f\xa5\xe7\x9c\x8b)\n");
                        m_terminal->reset_color();
                    }

                    transition_to(TuiState::STREAMING);

                    m_stream_buf->start();
                    m_formatter->reset();
                }

                if (m_viewing_thinking.load()) {
                    // H-1 修复：overlay 期间缓冲到 m_pending_content，收起时统一 flush
                    // 避免直接 feed() 导致内容写入终端破坏思考视图显示
                    m_pending_content += e.content_delta;
                } else {
                    m_formatter->feed(e.content_delta);
                }
            }

            // 注意：StreamTokenEvent.token_count 在 chat_session.cpp:195 中硬编码为 0，
            //       此处不累加 token，避免与 StreamDoneEvent 的 generated_tokens 重复计数。
        })
    );

    // ---- StepDoneEvent → 单步结束（P3 新增，ReAct 中间步骤）----
    // 轻量收尾：spinner 停止、formatter flush、思考标记输出
    // 不触发会话级动作（token 统计、状态转 IDLE、光标复位）
    m_token_step_done = std::make_unique<EventToken>(
        bus.subscribe<StepDoneEvent>([this](const StepDoneEvent& /*e*/) {
            if (m_spinner_active.load()) {
                m_terminal->spinner_stop();
                m_spinner_active.store(false);
            }

            // 刷新 StreamingBuffer 和 OutputFormatter
            m_formatter->flush();
            m_stream_buf->stop();

            // 如果还在思考状态且有推理内容，输出 ● 思考 Ns 标记
            if (m_state_machine.current() == TuiState::THINKING && !m_reasoning_buffer.empty()) {
                m_terminal->set_color(ColorRole::Success);
                m_terminal->write(" \xe2\x97\x8f \xe6\x80\x9d\xe8\x80\x83 ");  // ● (绿色)
                m_terminal->write(std::to_string(m_thinking_seconds.load()));
                m_terminal->write("s (ctrl+o \xe6\x9f\xa5\xe7\x9c\x8b)\n");
                m_terminal->reset_color();
            }
            // 不输出换行、不显示 token 统计、不更新 token_stats、
            // 不 increment_message_count、不转 IDLE、不光标复位
            // —— 这些是会话级结束动作，由 StreamDoneEvent 处理
        })
    );

    // ---- StreamDoneEvent → 会话完成（整个推理结束）----
    m_token_done = std::make_unique<EventToken>(
        bus.subscribe<StreamDoneEvent>([this](const StreamDoneEvent& e) {
            if (m_spinner_active.load()) {
                m_terminal->spinner_stop();
                m_spinner_active.store(false);
            }

            // 刷新 StreamingBuffer 和 OutputFormatter
            m_formatter->flush();
            m_stream_buf->stop();

            // 如果还在思考状态且有推理内容，输出 ● 思考 Ns 标记
            if (m_state_machine.current() == TuiState::THINKING && !m_reasoning_buffer.empty()) {
                m_terminal->set_color(ColorRole::Success);
                m_terminal->write(" \xe2\x97\x8f \xe6\x80\x9d\xe8\x80\x83 ");  // ● (绿色)
                m_terminal->write(std::to_string(m_thinking_seconds.load()));
                m_terminal->write("s (ctrl+o \xe6\x9f\xa5\xe7\x9c\x8b)\n");
                m_terminal->reset_color();
            }

            m_terminal->write("\n");

            // 显示 token 统计
            if (e.generated_tokens > 0) {
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
            }

            // Context 上下文占用：
            // - provider 返回 usage 时：用 prompt + cache 部分 + generated 覆盖（最准确）
            //   Anthropic 命中 prompt cache 时 prompt_tokens 不含 cache 部分，需单独累加
            // - provider 不返回 usage 时：用 compact::estimate_messages_tokens 估算响应内容
            if (e.prompt_tokens > 0 || e.generated_tokens > 0) {
                m_token_stats.update_from_usage(e.prompt_tokens,
                                                e.generated_tokens,
                                                e.cache_creation_input_tokens,
                                                e.cache_read_input_tokens);
            } else {
                m_token_stats.add_response_estimate(e.full_content, e.full_reasoning);
            }
            m_status_bar->set_cache_read_tokens(m_token_stats.cache_read_tokens());

            m_token_stats.increment_message_count();
            m_terminal->mark_cursor_left_output();
            transition_to(TuiState::IDLE);
            m_status_bar->set_state(TuiState::IDLE);
            m_status_bar->set_token_count(m_token_stats.total_tokens());
            m_status_bar->render();
            // 流结束，光标复位到输入行
            {
                int h = m_terminal->get_terminal_height();
                int input_row = h - 1;
                if (input_row < 1) input_row = 1;
                m_terminal->cursor_to_pos(input_row, INPUT_PROMPT_COL);
            }
        })
    );

    // ---- StreamErrorEvent → 错误 ----
    m_token_error = std::make_unique<EventToken>(
        bus.subscribe<StreamErrorEvent>([this](const StreamErrorEvent& e) {
            if (m_spinner_active.load()) {
                m_terminal->spinner_stop();
                m_spinner_active.store(false);
            }
            m_stream_buf->stop();
            m_formatter->reset();

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
                m_terminal->cursor_to_pos(input_row, INPUT_PROMPT_COL);
            }
            transition_to(TuiState::ERROR);
            m_status_bar->set_state(TuiState::ERROR);
            m_status_bar->render();
        })
    );

    // ---- AgentStepEvent ----
    // 用户反馈 step 计数提示多余，已移除文本输出
    m_token_step = std::make_unique<EventToken>(
        bus.subscribe<AgentStepEvent>([this](const AgentStepEvent& /*e*/) {
            // 无渲染输出
        })
    );

    // ---- ToolCallEvent ----
    m_token_tool_call = std::make_unique<EventToken>(
        bus.subscribe<ToolCallEvent>([this](const ToolCallEvent& e) {
            auto icon = get_tool_icon(e.tool_name);
            const int indent_level = m_tool_tracker.on_tool_call(e.call_id, e.tool_name, e.arguments);
            std::string indent(indent_level * 2, ' ');

            m_terminal->set_color(ColorRole::ToolName);
            m_terminal->write(std::format("{}{} {} (ctrl+o to view)\n",
                indent, icon.icon, e.tool_name));
            m_terminal->reset_color();

            // 更新 StatusBar
            m_status_bar->set_tool_name(e.tool_name);
            transition_to(TuiState::TOOL_RUNNING);
            m_status_bar->set_state(TuiState::TOOL_RUNNING);
            m_status_bar->render();
        })
    );

    // ---- ToolResultEvent ----
    m_token_tool_result = std::make_unique<EventToken>(
        bus.subscribe<ToolResultEvent>([this](const ToolResultEvent& e) {
            auto [info_opt, new_indent] = m_tool_tracker.on_tool_result(e.call_id);
            std::string indent(new_indent * 2, ' ');

            // 成功/失败标记
            const char* marker = e.is_error
                ? "\xe2\x9c\x97"  // ✗
                : "\xe2\x9c\x93"; // ✓
            ColorRole marker_color = e.is_error ? ColorRole::Failure : ColorRole::Success;

            bool has_info = info_opt.has_value();
            ToolCallTracker::ToolCallInfo info;
            if (has_info) {
                info = std::move(*info_opt);
            }

            m_terminal->set_color(marker_color);
            m_terminal->write(indent + "  ");
            m_terminal->write(marker);
            m_terminal->reset_color();

            if (has_info && is_code_tool(info.tool_name) && !e.is_error) {
                // 代码类工具: 走高亮渲染 (输出已带 ANSI, 不再设 color)
                std::string rendered = render_code_tool_result(
                    info.tool_name, info.arguments, e.result, indent, e.is_error);
                if (!rendered.empty()) {
                    m_terminal->write(rendered);
                    // 防御性 reset (高亮输出末尾理论上已 reset)
                    m_terminal->reset_color();
                } else {
                    // render_code_tool_result 返回空 = 无 diff / 兜底, 走 preview
                    std::string preview = e.result;
                    if (preview.length() > 200) {
                        preview.replace(200, std::string::npos, "...");
                    }
                    m_terminal->set_color(ColorRole::ToolOutput);
                    m_terminal->write(" \xe2\x8e\xbf  " + preview + "\n");  // ⎿
                    m_terminal->reset_color();
                }
            } else {
                // 其他工具 / 错误: 走原 200 字符 preview 逻辑
                std::string preview = e.result;
                if (preview.length() > 200) {
                    preview.replace(200, std::string::npos, "...");
                }
                m_terminal->set_color(ColorRole::ToolOutput);
                m_terminal->write(" \xe2\x8e\xbf  " + preview + "\n");  // ⎿
                m_terminal->reset_color();
            }

            transition_to(TuiState::STREAMING);
            m_status_bar->set_state(TuiState::STREAMING);
            m_status_bar->render();
        })
    );

    // ---- AgentDoneEvent ----
    // 用户反馈完成提示多余，已移除文本输出；保留 m_tool_tracker.reset_indent()
    m_token_agent_done = std::make_unique<EventToken>(
        bus.subscribe<AgentDoneEvent>([this](const AgentDoneEvent& /*e*/) {
            m_tool_tracker.reset_indent();
        })
    );
}

void ChatRenderer::stop() {
    // D-4：复用 Terminal 的 DI 路径，消除对 EventBus 单例的直接依赖
    auto& bus = m_terminal->event_bus();

    if (m_spinner_active.load()) {
        m_terminal->spinner_stop();
        m_spinner_active.store(false);
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
    if (m_token_step_done && m_token_step_done->is_valid()) {
        bus.unsubscribe<StepDoneEvent>(*m_token_step_done);
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
    if (m_token_user_input && m_token_user_input->is_valid()) {
        bus.unsubscribe<UserInputEvent>(*m_token_user_input);
    }
}

void ChatRenderer::transition_to(TuiState new_state) {
    m_state_machine.transition_to(new_state);
    if (m_state_machine.current() != new_state) {
        m_state_machine.force_state(new_state);
    }
}

void ChatRenderer::toggle_thinking_view() {
    if (m_reasoning_buffer.empty()) return;

    if (!m_viewing_thinking.load()) {
        // ---- 展开思考视图：快照对话区 + 轻量样式渲染 ----
        m_viewing_thinking.store(true);

        int height = m_terminal->get_terminal_height();
        int scroll_bottom = height - 3;
        if (scroll_bottom < 1) scroll_bottom = 1;

        // 快照当前对话区（同时设置 overlay active，阻止思考内容写入 DisplayBuffer）
        // 收起时 end_overlay() 会从快照恢复对话，避免清屏丢失历史
        m_terminal->begin_overlay(1, scroll_bottom);

        // 重置 scroll region，允许思考视图使用全屏
        m_terminal->reset_scroll_region();

        // 清屏
        m_terminal->write("\x1b[2J\x1b[H");

        // 标题行：● 思考 Ns (ctrl+o 返回) — 移除 ┌─┐ 硬边框，仅颜色区分
        m_terminal->set_color(ColorRole::ThinkingBlock);
        m_terminal->write(std::format(" \xe2\x97\x8f \xe6\x80\x9d\xe8\x80\x83 {}s (ctrl+o \xe8\xbf\x94\xe5\x9b\x9e)\n",
            m_thinking_seconds.load()));
        m_terminal->reset_color();

        // 思考内容：缩进式块，走 markdown 渲染
        std::string rendered = render_markdown_block(m_reasoning_buffer);
        // 每行添加 2 空格缩进，与标题区分（替代硬边框的视觉分隔）
        std::string indented;
        indented.reserve(rendered.size() + 64);
        bool at_line_start = true;
        for (char c : rendered) {
            if (at_line_start && c != '\n') {
                indented += "  ";
            }
            indented += c;
            at_line_start = (c == '\n');
        }
        m_terminal->set_color(ColorRole::Reasoning);
        m_terminal->write(indented);
        m_terminal->reset_color();

        // 底部提示（移除 └─┘ 硬边框）
        m_terminal->write("\n");
        m_terminal->set_color(ColorRole::Dim);
        m_terminal->write("  (ctrl+o \xe8\xbf\x94\xe5\x9b\x9e)\n");
        m_terminal->reset_color();
    } else {
        // ---- 收起思考视图：从快照恢复对话区 ----
        m_viewing_thinking.store(false);

        // H-2 修复：先 end_overlay 恢复对话内容，再 setup_scroll_region
        // end_overlay() 使用 platform->write_output 直接写入，不受 scroll region 影响
        // 若先 setup_scroll_region() 会将光标定位到 (1,1)，end_overlay() 的 DECSC 会
        // 保存这个错误的光标位置，导致收起后光标停在左上角而非对话末尾
        m_terminal->end_overlay();

        // 恢复 scroll region（在对话内容恢复之后）
        m_terminal->setup_scroll_region();

        // 光标归位到输出区底部（对话末尾），确保下次输入时光标位置正确
        m_terminal->cursor_to_output();

        // H-1 修复：flush overlay 期间缓冲的正文内容到 formatter
        // 此时 scroll region 已恢复，feed() 会正确写入终端并进入 DisplayBuffer
        if (!m_pending_content.empty()) {
            m_formatter->feed(m_pending_content);
            m_pending_content.clear();
        }

        // 流式输出进行中时附加提示
        if (m_state_machine.current() == TuiState::STREAMING) {
            m_terminal->set_color(ColorRole::Dim);
            m_terminal->write("  [streaming in progress...]\n");
            m_terminal->reset_color();
        }

        // 重绘状态栏
        m_status_bar->render();
    }
}

} // namespace tui
