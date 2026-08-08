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
#include "core/events/system_events.h"
#include "core/events/agent_events.h"  // CacheDiagnosticsEvent
#include "core/task/task_events.h"
#include "agent/message/types.h"

#include <format>
#include <cassert>
#include <chrono>
#include <thread>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>
#include <string>
#include <cstring>
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

/// @brief 判断是否为 Shell 类工具 (Bash/PowerShell)
bool is_shell_tool(const std::string& tool_name) {
    return tool_name == "Bash" || tool_name == "PowerShell";
}

/// @brief 渲染工具结果摘要（恢复会话场景的兜底显示）
/// @details 多行结果只显示行数，少行结果显示内容（截断到 200 字符）。
///          调用方已写入 ✓/✗ marker，本函数返回从 " ⎿  " 开始的字符串。
std::string render_tool_summary(const std::string& content)
{
    // 统计行数
    int line_count = 0;
    {
        std::string cur;
        for (char c : content) {
            if (c == '\n') { ++line_count; cur.clear(); }
            else cur.push_back(c);
        }
        if (!cur.empty()) ++line_count;
    }

    const std::string arrow = "\xe2\x8e\xbf";  // ⎿
    const std::string dim = "\x1b[2m";
    const std::string reset = "\x1b[0m";

    if (line_count > 3) {
        return " " + arrow + "  " + dim +
               std::format("({} lines)", line_count) + reset + "\n";
    }
    std::string preview = content;
    if (preview.length() > 200) {
        preview.replace(200, std::string::npos, "...");
    }
    while (!preview.empty() && preview.back() == '\n') {
        preview.pop_back();
    }
    return " " + arrow + "  " + dim + preview + reset + "\n";
}

/// @brief 从 ToolCallEvent.arguments (JSON) 解析 command 字段
/// @return command 字符串; 解析失败或字段不存在返回空
std::string extract_command(const std::string& arguments_json) {
    if (arguments_json.empty()) return {};
    try {
        auto j = nlohmann::json::parse(arguments_json);
        if (j.contains("command") && j["command"].is_string()) {
            return j["command"].get<std::string>();
        }
    } catch (...) {
        // 解析失败忽略
    }
    return {};
}

/// @brief 渲染 Shell 工具的结果 (Bash/PowerShell)
/// @details 格式:
///   ⎿  <command>
///       <output line 1>
///       <output line 2>
///       ...
/// 输出内容与 command 对齐 (前面补空格)。
/// 注意: 调用方已写入 ✓/✗ marker, 本函数输出从 " ⎿  " 开始。
/// @param arguments_json  工具调用参数 JSON (用于提取 command)
/// @param result          format_result() 生成的工具结果 (含 <stdout>/<stderr>/<error> 标签)
/// @param indent          当前缩进字符串
std::string render_shell_tool_result(
    const std::string& arguments_json,
    const std::string& result,
    const std::string& indent)
{
    const std::string command = extract_command(arguments_json);

    constexpr std::string_view dim = "\x1b[2m";
    constexpr std::string_view reset = "\x1b[0m";
    constexpr int MAX_DISPLAY_LINES = 60;

    std::ostringstream os;
    const std::string arrow = "\xe2\x8e\xbf";  // ⎿

    // 对齐前缀: 与 command 起始列对齐
    // command 起始列 = indent + "  "(2) + marker(1) + " "(1) + arrow(1) + "  "(2) = indent + 7
    const std::string align_prefix = indent + "       ";  // 7 spaces

    // 第一行: ⎿  <command>
    os << " " << arrow << "  ";
    if (!command.empty()) {
        os << dim << command << reset;
    } else {
        os << dim << "(shell output)" << reset;
    }
    os << "\n";

    // 解析 result, 去掉 <stdout>/<stderr>/<error> 标签, 逐行收集内容
    std::vector<std::string> content_lines;
    {
        std::string cur;
        auto flush = [&]() {
            // 去掉行尾 \r
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            // 跳过纯标签行
            if (cur == "<stdout>" || cur == "</stdout>" ||
                cur == "<stderr>" || cur == "</stderr>") {
                cur.clear();
                return;
            }
            // 去掉行内 <error>/<stdout>/<stderr> 及闭合标签 (如 <error>Command exited with code 1</error>)
            std::string cleaned = cur;
            for (const auto* tag : {"<error>", "<stdout>", "<stderr>",
                                     "</error>", "</stdout>", "</stderr>"}) {
                size_t pos = cleaned.find(tag);
                while (pos != std::string::npos) {
                    cleaned.erase(pos, std::strlen(tag));
                    pos = cleaned.find(tag, pos);
                }
            }
            content_lines.push_back(std::move(cleaned));
            cur.clear();
        };
        for (char c : result) {
            if (c == '\n') flush();
            else cur.push_back(c);
        }
        if (!cur.empty()) flush();
    }

    // 去掉首尾空行
    while (!content_lines.empty() && content_lines.front().empty()) {
        content_lines.erase(content_lines.begin());
    }
    while (!content_lines.empty() && content_lines.back().empty()) {
        content_lines.pop_back();
    }

    // 限制最大显示行数
    const bool truncated = static_cast<int>(content_lines.size()) > MAX_DISPLAY_LINES;
    if (truncated) content_lines.resize(MAX_DISPLAY_LINES);

    // 输出内容行, 带对齐前缀
    for (const auto& line : content_lines) {
        os << align_prefix << line << "\n";
    }
    if (truncated) {
        os << align_prefix << dim
           << "(... truncated, showing first " << MAX_DISPLAY_LINES << " lines)"
           << reset << "\n";
    }

    return os.str();
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

/// @brief 从 FileRead 行号前缀 (如 "  123→") 中提取数字行号
/// @return 行号; 无法解析返回 -1
int extract_fileread_line_number(const std::string& prefix) {
    size_t i = 0;
    while (i < prefix.size() && (prefix[i] == ' ' || prefix[i] == '\t')) ++i;
    if (i >= prefix.size() || prefix[i] < '0' || prefix[i] > '9') return -1;
    int num = 0;
    while (i < prefix.size() && prefix[i] >= '0' && prefix[i] <= '9') {
        num = num * 10 + (prefix[i] - '0');
        ++i;
    }
    return num;
}

/// @brief 计算 │N│ 格式行号渲染所需的宽度 (按最大行号位数)
/// @param max_line_num  最大行号 (若 <=0 则按 1 位处理)
/// @return 行号字段的字符宽度 (最少 1)
int calc_line_num_width(int max_line_num) {
    if (max_line_num < 1) max_line_num = 1;
    int width = 1;
    for (int n = max_line_num; n >= 10; n /= 10) ++width;
    return width;
}

/// @brief 渲染 │N  格式的行号前缀 (│ + 右对齐数字 + 两空格), 带 Dim 色
/// @param line_num      行号 (1-based)
/// @param num_width     行号字段宽度 (由 calc_line_num_width 计算)
/// @param dim           Dim 色 ANSI 序列
/// @param reset         RESET ANSI 序列
std::string format_line_num_prefix(int line_num, int num_width,
                                    std::string_view dim, std::string_view reset) {
    const std::string box_v = "\xe2\x94\x82";  // │ U+2502
    std::string num_str = std::to_string(line_num);
    std::string padding;
    if (static_cast<int>(num_str.size()) < num_width) {
        padding.append(num_width - num_str.size(), ' ');
    }
    std::string out;
    out += dim;
    out += box_v;
    out += padding;
    out += num_str;
    out += reset;
    out += "  ";
    return out;
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
///   - FileRead: 剥离行号 → 整体高亮代码 → 以 │N  格式重新加行号 (右对齐, Dim 色)
///               末尾元数据行 "(N of M lines shown)" 不高亮, 以 Dim 色输出
///   - Write/Edit: 结果格式为 "状态文本\n\n<diff>", 拆分:
///       状态行原样输出 (Dim 色), diff 部分走 highlight_diff, 每行加 │N  序号
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

        // 每行加 │N  格式行号 (顺序编号, 右对齐) — 文件工具不用 ⎿ 包裹
        const int num_width = calc_line_num_width(static_cast<int>(diff_lines.size()));
        for (size_t i = 0; i < diff_lines.size(); ++i) {
            const auto& l = diff_lines[i];
            os << indent << "  "
               << format_line_num_prefix(static_cast<int>(i + 1), num_width, dim, reset);
            os << l;
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

    // 输出状态行: "⎿ The file xxx has been read successfully."
    // 与 Write/Edit 的状态行保持一致的视觉格式
    {
        std::string display_path = file_path.empty() ? std::string{"file"} : file_path;
        os << indent << "  " << arrow << " " << dim
           << "The file " << display_path << " has been read successfully."
           << reset << "\n";
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

    // 剥离行号前缀, 拼成纯代码块; 同时提取实际行号用于 │N  格式渲染
    std::vector<int> line_nums;
    line_nums.reserve(lines.size());
    int max_line_num = 0;
    std::string code_blob;
    for (const auto& l : lines) {
        std::string prefix, code;
        split_fileread_line(l, prefix, code);
        int n = extract_fileread_line_number(prefix);
        line_nums.push_back(n);
        if (n > max_line_num) max_line_num = n;
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

    // 组装代码行: indent + "  " + │N(Dim,右对齐) + 两空格 + 高亮代码行 + RESET + "\n"
    // 文件工具不用 ⎿ 包裹, 直接 │N  code
    const int num_width = calc_line_num_width(max_line_num);
    for (size_t i = 0; i < hl_lines.size(); ++i) {
        os << indent << "  ";
        if (i < line_nums.size() && line_nums[i] > 0) {
            os << format_line_num_prefix(line_nums[i], num_width, dim, reset);
            os << hl_lines[i];
            os << reset;
        } else {
            os << hl_lines[i];
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
                    m_thinking_indicator_shown = false;
                    transition_to(TuiState::THINKING);
                    m_thinking_start_time = std::chrono::steady_clock::now();
                    m_thinking_seconds.store(0);
                    m_reasoning_buffer.clear();
                    m_thinking_marker_physical_row = 0;
                    m_thinking_marker_offset = 0;
                    m_thinking_expanded = false;
                    m_thinking_used_full_overlay = false;

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
    // 本地命令（is_local_command=true）不发送给 LLM，跳过 token 统计累加
    m_token_user_input = std::make_unique<EventToken>(
        bus.subscribe<UserInputEvent>([this](const UserInputEvent& e) {
            if (e.is_local_command) return;
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
                    m_thinking_indicator_shown = true;
                    m_thinking_marker_offset = 1;  // "思考中" 行会被覆盖，但 DisplayBuffer 仍保留

                    transition_to(TuiState::THINKING);
                    m_status_bar->set_state(TuiState::THINKING);
                    m_status_bar->render();
                }

                m_reasoning_buffer += e.reasoning_delta;

                // H-1 修复：overlay 期间不写入终端（避免破坏思考视图显示）
                // 思考内容已追加到 m_reasoning_buffer，用户下次展开时可看到完整内容
                // M-1: 统一通过 is_overlay_active() 查询，消除与 Terminal::write() 的状态非原子窗口
                if (m_terminal->is_overlay_active()) {
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
                        // 覆盖之前的 "● 思考中..." 临时候选标记（ANSI: 上移1行+清行）
                        if (m_thinking_indicator_shown) {
                            m_terminal->write("\x1b[1A\x1b[2K");
                            m_thinking_indicator_shown = false;
                        }
                        m_terminal->set_color(ColorRole::Success);
                        m_terminal->write(" \xe2\x97\x8f \xe6\x80\x9d\xe8\x80\x83 ");
                        m_terminal->write(std::to_string(m_thinking_seconds.load()));
                        m_terminal->write("s (ctrl+o \xe6\x9f\xa5\xe7\x9c\x8b)\n");
                        // 记录标记物理行号（此时标记行已入 DisplayBuffer，row_count 含标记行）
                        m_thinking_marker_physical_row = m_terminal->display_buffer_row_count();
                        m_terminal->write("\n");
                        m_terminal->reset_color();
                    }

                    transition_to(TuiState::STREAMING);

                    m_stream_buf->start();
                    m_formatter->reset();
                }

                if (m_terminal->is_overlay_active()) {
                    // H-1 修复：overlay 期间缓冲到 m_pending_content，收起时统一 flush
                    // 避免直接 feed() 导致内容写入终端破坏思考视图显示
                    // M-1: 统一通过 is_overlay_active() 查询，保证与 Terminal::write() 状态一致
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
                // 覆盖之前的 "● 思考中..." 临时候选标记
                if (m_thinking_indicator_shown) {
                    m_terminal->write("\x1b[1A\x1b[2K");
                    m_thinking_indicator_shown = false;
                }
                m_terminal->set_color(ColorRole::Success);
                m_terminal->write(" \xe2\x97\x8f \xe6\x80\x9d\xe8\x80\x83 ");  // ● (绿色)
                m_terminal->write(std::to_string(m_thinking_seconds.load()));
                m_terminal->write("s (ctrl+o \xe6\x9f\xa5\xe7\x9c\x8b)\n");
                m_thinking_marker_physical_row = m_terminal->display_buffer_row_count();
                m_terminal->write("\n");
                m_terminal->reset_color();
            }
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
                // 覆盖之前的 "● 思考中..." 临时候选标记
                if (m_thinking_indicator_shown) {
                    m_terminal->write("\x1b[1A\x1b[2K");
                    m_thinking_indicator_shown = false;
                }
                m_terminal->set_color(ColorRole::Success);
                m_terminal->write(" \xe2\x97\x8f \xe6\x80\x9d\xe8\x80\x83 ");  // ● (绿色)
                m_terminal->write(std::to_string(m_thinking_seconds.load()));
                m_terminal->write("s (ctrl+o \xe6\x9f\xa5\xe7\x9c\x8b)\n");
                m_thinking_marker_physical_row = m_terminal->display_buffer_row_count();
                m_terminal->write("\n");
                m_terminal->reset_color();
            } else {
                m_terminal->write("\n");
            }

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
            // - 本地命令（is_local_command=true）：跳过 token 统计累加，避免 /help 等命令
            //   输出被误算作 LLM 响应
            if (e.is_local_command) {
                // 本地命令输出不累加 token，但仍需转 IDLE 状态和光标复位
            } else if (e.prompt_tokens > 0 || e.generated_tokens > 0) {
                m_token_stats.update_from_usage(e.prompt_tokens,
                                                e.generated_tokens,
                                                e.cache_creation_input_tokens,
                                                e.cache_read_input_tokens,
                                                e.prompt_cache_hit_tokens,
                                                e.prompt_cache_miss_tokens);
            } else {
                m_token_stats.add_response_estimate(e.full_content, e.full_reasoning);
            }
            m_status_bar->set_cache_read_tokens(m_token_stats.cache_read_tokens());
            m_status_bar->set_ds_cache_hit_rate(m_token_stats.ds_cache_hit_rate());

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

            if (has_info && is_shell_tool(info.tool_name)) {
                // Shell 工具 (Bash/PowerShell): 显示命令 + 对齐输出
                std::string rendered = render_shell_tool_result(
                    info.arguments, e.result, indent);
                m_terminal->write(rendered);
                m_terminal->reset_color();
            } else if (has_info && is_code_tool(info.tool_name) && !e.is_error) {
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

    // ---- TerminalResizeEvent ----
    // resize 后：StatusBar 的 m_last_rendered_row 已失效，需擦除旧行并强制重绘
    m_token_resize = std::make_unique<EventToken>(
        bus.subscribe<TerminalResizeEvent>([this](const TerminalResizeEvent& /*e*/) {
            // invalidate_last_row() 让下次 render() 擦除旧行（即使内容相同也强制重绘）
            m_status_bar->invalidate_last_row();
            m_status_bar->render();
        })
    );

    // ---- TaskCompletedEvent（后台任务完成通知）----
    // BashTool run_in_background=true 启动的后台任务完成后触发
    m_token_task_completed = std::make_unique<EventToken>(
        bus.subscribe<TaskCompletedEvent>([this](const TaskCompletedEvent& e) {
            // 仅显示后台任务（前缀 "bash:"），避免与其他任务重复
            if (e.task_name.rfind("bash:", 0) != 0) return;
            m_terminal->set_color(ColorRole::Success);
            m_terminal->write(std::format(
                "[bg] {} completed ({:.0f}ms)\n", e.task_name, e.duration_ms));
            m_terminal->reset_color();
        })
    );

    // ---- TaskFailedEvent（后台任务失败通知）----
    m_token_task_failed = std::make_unique<EventToken>(
        bus.subscribe<TaskFailedEvent>([this](const TaskFailedEvent& e) {
            if (e.task_name.rfind("bash:", 0) != 0) return;
            m_terminal->set_color(ColorRole::Failure);
            m_terminal->write(std::format(
                "[bg] {} failed: {} ({:.0f}ms)\n",
                e.task_name, e.error_message, e.duration_ms));
            m_terminal->reset_color();
        })
    );

    // ---- CacheDiagnosticsEvent → 缓存劣化归因提示 ----
    // 仅当 prefix_changed=true 时显示，帮助用户理解命中率下降原因
    m_token_cache_diag = std::make_unique<agent::EventToken>(
        bus.subscribe<CacheDiagnosticsEvent>([this](const CacheDiagnosticsEvent& e) {
            if (!e.prefix_changed) return;
            std::string reason_str;
            for (size_t i = 0; i < e.reasons.size(); ++i) {
                if (i > 0) reason_str += "+";
                reason_str += e.reasons[i];
            }
            m_terminal->set_color(ColorRole::ContextWarning);
            m_terminal->write(std::format(
                "  [cache] prefix changed ({}) | miss {} tokens\n",
                reason_str, e.cache_miss_tokens));
            m_terminal->reset_color();
        })
    );

    // ---- AskUserRequestEvent → 设置 pending 请求并唤醒主循环 ----
    // 事件泵线程 drain 到 AskUser 请求时，存入 Terminal 的 pending 槽，
    // 并通过 platform wake_event 唤醒阻塞在 read_char 的主循环。
    // 主循环取出 pending 后弹出 ChoicePanel 模态，结果回填 promise 唤醒工作线程。
    m_token_ask_user = std::make_unique<agent::EventToken>(
        bus.subscribe<AskUserRequestEvent>([this](const AskUserRequestEvent& e) {
            tui::detail::PendingAskRequest req;
            req.questions = e.questions;
            req.result_promise = e.result_promise;
            req.cancel_flag = e.cancel_flag;
            m_terminal->set_pending_ask(std::move(req));
        })
    );

    // ---- AskUserTimeoutEvent → 唤醒主循环关闭 ChoicePanel ----
    // 工作线程超时后置位 cancel_flag 并发布本事件，
    // 此处唤醒主循环使 read_char 返回 KEY_WAKE，run_choice_panel 检查 cancel_flag 后退出。
    m_token_ask_timeout = std::make_unique<agent::EventToken>(
        bus.subscribe<AskUserTimeoutEvent>([this](const AskUserTimeoutEvent& /*e*/) {
            m_terminal->wake_main_loop();
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
    if (m_token_resize && m_token_resize->is_valid()) {
        bus.unsubscribe<TerminalResizeEvent>(*m_token_resize);
    }
    if (m_token_task_completed && m_token_task_completed->is_valid()) {
        bus.unsubscribe<TaskCompletedEvent>(*m_token_task_completed);
    }
    if (m_token_task_failed && m_token_task_failed->is_valid()) {
        bus.unsubscribe<TaskFailedEvent>(*m_token_task_failed);
    }
    if (m_token_cache_diag && m_token_cache_diag->is_valid()) {
        bus.unsubscribe<CacheDiagnosticsEvent>(*m_token_cache_diag);
    }
    if (m_token_ask_user && m_token_ask_user->is_valid()) {
        bus.unsubscribe<AskUserRequestEvent>(*m_token_ask_user);
    }
    if (m_token_ask_timeout && m_token_ask_timeout->is_valid()) {
        bus.unsubscribe<AskUserTimeoutEvent>(*m_token_ask_timeout);
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

    if (!m_terminal->is_overlay_active()) {
        // ============================================================
        // 展开思考视图
        // ============================================================

        // 思考进行中（THINKING 状态）：用全屏 overlay（实时追加 reasoning_delta）
        // 此时标记 "● 思考 Ns" 尚未输出，m_thinking_marker_physical_row 无效
        if (m_state_machine.current() == TuiState::THINKING) {
            m_thinking_used_full_overlay = true;
            int height = m_terminal->get_terminal_height();
            int scroll_bottom = height - 3;
            if (scroll_bottom < 1) scroll_bottom = 1;

            m_terminal->begin_overlay(1, scroll_bottom);
            m_terminal->reset_scroll_region();
            m_terminal->write("\x1b[2J\x1b[H");

            // 标题行
            m_terminal->set_color(ColorRole::ThinkingBlock);
            m_terminal->write(std::format(" \xe2\x97\x8f \xe6\x80\x9d\xe8\x80\x83 {}s (ctrl+o \xe8\xbf\x94\xe5\x9b\x9e)\n",
                m_thinking_seconds.load()));
            m_terminal->reset_color();

            // 已累积的思考内容
            std::string rendered = render_markdown_block(m_reasoning_buffer);
            std::string indented;
            indented.reserve(rendered.size() + 64);
            std::istringstream iss(rendered);
            std::string line;
            while (std::getline(iss, line)) {
                if (!line.empty()) indented += "  ";
                indented += line;
                indented += "\n";
            }
            m_terminal->set_color(ColorRole::Reasoning);
            m_terminal->write(indented);
            m_terminal->reset_color();

            m_terminal->write("\n");
            m_terminal->set_color(ColorRole::Dim);
            m_terminal->write("  (ctrl+o \xe8\xbf\x94\xe5\x9b\x9e)\n");
            m_terminal->reset_color();
            return;
        }

        // 思考已结束：尝试局部 overlay（保留标记行上方对话内容）
        int height = m_terminal->get_terminal_height();
        int scroll_bottom = height - 3;
        if (scroll_bottom < 1) scroll_bottom = 1;
        int scroll_h = scroll_bottom;

        int total_rows = m_terminal->display_buffer_row_count();
        // 计算标记屏幕行号（底部对齐映射 + 偏差修正）
        // DisplayBuffer snapshot 映射: physical = screen_row + total_rows - scroll_h
        // 逆映射: screen_row = physical - total_rows + scroll_h
        // m_thinking_marker_offset 修正 "思考中..." 覆盖导致的 DisplayBuffer 多 1 行
        int marker_screen_row = m_thinking_marker_physical_row > 0
            ? (m_thinking_marker_physical_row - total_rows + scroll_h - m_thinking_marker_offset)
            : 0;

        if (marker_screen_row >= 1) {
            // ---- 局部 overlay：标记行在屏幕内，保留标记行上方内容 ----
            // 快照标记行下方到 scroll_bottom 的区域
            m_terminal->begin_overlay(marker_screen_row + 1, scroll_bottom);

            // 定位光标到标记行，更新标记文本为 "(ctrl+o 收起)"
            {
                char cmd[32];
                snprintf(cmd, sizeof(cmd), "\x1b[%d;1H\x1b[2K", marker_screen_row);
                m_terminal->write(cmd);
                m_terminal->set_color(ColorRole::Success);
                m_terminal->write(std::format(" \xe2\x97\x8f \xe6\x80\x9d\xe8\x80\x83 {}s (ctrl+o \xe6\x94\xb6\xe8\xb5\xb7)",
                    m_thinking_seconds.load()));
                m_terminal->reset_color();
            }

            // 定位光标到标记行下方，写入思考内容
            {
                char cmd[32];
                snprintf(cmd, sizeof(cmd), "\x1b[%d;1H", marker_screen_row + 1);
                m_terminal->write(cmd);
            }

            // 思考内容：markdown 渲染 + 2 空格缩进
            std::string rendered = render_markdown_block(m_reasoning_buffer);
            std::string indented;
            indented.reserve(rendered.size() + 64);
            std::istringstream iss(rendered);
            std::string line;
            while (std::getline(iss, line)) {
                if (!line.empty()) indented += "  ";
                indented += line;
                indented += "\n";
            }
            m_terminal->set_color(ColorRole::Reasoning);
            m_terminal->write(indented);
            m_terminal->reset_color();

            // 底部提示
            m_terminal->set_color(ColorRole::Dim);
            m_terminal->write("  (ctrl+o \xe6\x94\xb6\xe8\xb5\xb7)\n");
            m_terminal->reset_color();

            m_thinking_expanded = true;
        } else {
            // ---- fallback：标记滚出屏幕，用全屏 overlay ----
            m_thinking_used_full_overlay = true;

            m_terminal->begin_overlay(1, scroll_bottom);
            m_terminal->reset_scroll_region();
            m_terminal->write("\x1b[2J\x1b[H");

            m_terminal->set_color(ColorRole::ThinkingBlock);
            m_terminal->write(std::format(" \xe2\x97\x8f \xe6\x80\x9d\xe8\x80\x83 {}s (ctrl+o \xe8\xbf\x94\xe5\x9b\x9e)\n",
                m_thinking_seconds.load()));
            m_terminal->reset_color();

            std::string rendered = render_markdown_block(m_reasoning_buffer);
            std::string indented;
            indented.reserve(rendered.size() + 64);
            std::istringstream iss(rendered);
            std::string line;
            while (std::getline(iss, line)) {
                if (!line.empty()) indented += "  ";
                indented += line;
                indented += "\n";
            }
            m_terminal->set_color(ColorRole::Reasoning);
            m_terminal->write(indented);
            m_terminal->reset_color();

            m_terminal->write("\n");
            m_terminal->set_color(ColorRole::Dim);
            m_terminal->write("  (ctrl+o \xe8\xbf\x94\xe5\x9b\x9e)\n");
            m_terminal->reset_color();
        }
    } else {
        // ============================================================
        // 收起思考视图
        // ============================================================

        if (m_thinking_expanded) {
            // ---- 局部 overlay 收起 ----
            // end_overlay 恢复标记行下方的快照内容
            m_terminal->end_overlay();

            // 恢复标记文本为 "(ctrl+o 查看)"
            int scroll_h = m_terminal->get_terminal_height() - 3;
            int total_rows = m_terminal->display_buffer_row_count();
            int marker_screen_row = m_thinking_marker_physical_row - total_rows + scroll_h - m_thinking_marker_offset;

            if (marker_screen_row >= 1) {
                char cmd[32];
                snprintf(cmd, sizeof(cmd), "\x1b[%d;1H\x1b[2K", marker_screen_row);
                m_terminal->write(cmd);
                m_terminal->set_color(ColorRole::Success);
                m_terminal->write(std::format(" \xe2\x97\x8f \xe6\x80\x9d\xe8\x80\x83 {}s (ctrl+o \xe6\x9f\xa5\xe7\x9c\x8b)",
                    m_thinking_seconds.load()));
                m_terminal->reset_color();
            }

            // 光标归位到输出区底部
            m_terminal->cursor_to_output();

            // flush overlay 期间缓冲的正文
            if (!m_pending_content.empty()) {
                m_formatter->feed(m_pending_content);
                m_pending_content.clear();
            }

            m_thinking_expanded = false;
            m_status_bar->render();
        } else {
            // ---- 全屏 overlay 收起（旧逻辑）----
            // H-2 修复：先 end_overlay 恢复对话内容，再 setup_scroll_region
            m_terminal->end_overlay();
            m_terminal->setup_scroll_region();
            m_terminal->cursor_to_output();

            // H-1 修复：flush overlay 期间缓冲的正文内容到 formatter
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

            m_thinking_used_full_overlay = false;
            m_status_bar->render();
        }
    }
}

void ChatRenderer::replay_history(const std::vector<agent::ChatMessage>& messages, bool show_welcome) {
    // 清空输出区域并重置 formatter 状态
    m_formatter->reset();

    // 重置 ctrl+o 就地展开状态
    m_thinking_marker_physical_row = 0;
    m_thinking_marker_offset = 0;
    m_thinking_expanded = false;
    m_thinking_used_full_overlay = false;

    // 清屏流程：先重置 scroll region（使 \x1b[2J 清全屏，不受 region 限制），
    // 再全屏清屏 + 光标归位，最后重新设置 scroll region 并定位光标到顶部。
    // 这样可确保 overlay 快照恢复的欢迎横幅等旧内容被彻底清除。
    m_terminal->reset_scroll_region();
    m_terminal->write("\x1b[2J\x1b[H");
    m_terminal->setup_scroll_region();

    // 启动恢复时先渲染欢迎横幅（位于历史消息上方），/resume 切换时不重复显示
    if (show_welcome) {
        m_terminal->display_welcome();
    }

    // 预扫描：构建 tool_call_id → (tool_name, arguments_json) 映射
    // 用于 Tool 消息渲染时提取 shell 工具的 command 参数
    std::unordered_map<std::string, std::pair<std::string, std::string>> tool_args_map;
    for (const auto& m : messages) {
        if (m.role != agent::ChatMessage::Role::Assistant) continue;
        for (const auto& tu : m.tool_uses) {
            tool_args_map.emplace(tu.id,
                std::make_pair(tu.name, tu.input.dump()));
        }
    }

    // 渲染历史消息
    for (const auto& msg : messages) {
        switch (msg.role) {
            case agent::ChatMessage::Role::User: {
                // 用户消息："> 内容"（与 echo_input 风格一致）
                m_terminal->set_color(ColorRole::UserInput);
                m_terminal->write("> ");
                m_terminal->write(msg.content);
                m_terminal->write("\n");
                m_terminal->reset_color();
                break;
            }
            case agent::ChatMessage::Role::Assistant: {
                // 思考内容：有 reasoning_content 时显示 "● 思考 (ctrl+o 查看)" 标记
                // 并把最后一条 assistant 的思考内容回填到 m_reasoning_buffer（供 ctrl+o 展开）
                if (!msg.reasoning_content.empty()) {
                    m_terminal->set_color(ColorRole::Success);
                    m_terminal->write(" \xe2\x97\x8f \xe6\x80\x9d\xe8\x80\x83 (ctrl+o \xe6\x9f\xa5\xe7\x9c\x8b)\n");
                    m_terminal->reset_color();
                    m_reasoning_buffer = msg.reasoning_content;
                    m_thinking_seconds.store(0);  // 历史无秒数信息，显示 0s
                }
                // 工具调用标记
                for (const auto& tu : msg.tool_uses) {
                    auto icon = get_tool_icon(tu.name);
                    m_terminal->set_color(ColorRole::ToolName);
                    m_terminal->write(std::format("{} {} (ctrl+o to view)\n",
                        icon.icon, tu.name));
                    m_terminal->reset_color();
                }
                if (!msg.content.empty()) {
                    // 通过 OutputFormatter 渲染 markdown
                    m_formatter->feed(msg.content);
                    m_formatter->flush();
                    m_formatter->reset();
                    m_terminal->write("\n");
                }
                break;
            }
            case agent::ChatMessage::Role::Tool: {
                // 工具结果渲染（恢复会话场景）：
                //   - Shell 工具 (Bash/PowerShell): 复用 render_shell_tool_result
                //     显示命令 + 对齐输出，与实时执行格式一致
                //   - 其他工具: 走摘要逻辑 (行数或前 200 字符)
                const char* marker = msg.is_error
                    ? "\xe2\x9c\x97"  // ✗
                    : "\xe2\x9c\x93"; // ✓
                ColorRole marker_color = msg.is_error ? ColorRole::Failure : ColorRole::Success;
                m_terminal->set_color(marker_color);
                m_terminal->write("  ");
                m_terminal->write(marker);
                m_terminal->reset_color();

                // 查找对应的工具调用参数
                std::string tool_name = msg.tool_name;
                std::string arguments_json;
                auto it_args = tool_args_map.find(msg.tool_call_id);
                if (it_args != tool_args_map.end()) {
                    tool_name = it_args->second.first;
                    arguments_json = it_args->second.second;
                }

                if (is_shell_tool(tool_name) && !arguments_json.empty()) {
                    // Shell 工具：显示命令 + 对齐输出
                    std::string rendered = render_shell_tool_result(
                        arguments_json, msg.content, "");
                    m_terminal->write(rendered);
                    m_terminal->reset_color();
                } else if (is_code_tool(tool_name) && !msg.is_error && !arguments_json.empty()) {
                    // 代码工具：走高亮渲染（语法高亮 + 行号 / diff）
                    std::string rendered = render_code_tool_result(
                        tool_name, arguments_json, msg.content, "", msg.is_error);
                    if (!rendered.empty()) {
                        m_terminal->write(rendered);
                        m_terminal->reset_color();
                    } else {
                        // 无 diff / 兜底：走摘要
                        m_terminal->write(render_tool_summary(msg.content));
                        m_terminal->reset_color();
                    }
                } else {
                    // 其他工具 / 错误：摘要显示（行数或前 200 字符）
                    m_terminal->write(render_tool_summary(msg.content));
                    m_terminal->reset_color();
                }
                break;
            }
            case agent::ChatMessage::Role::System:
                // System 消息不渲染到输出区
                break;
        }
    }

    // 重绘状态栏
    m_status_bar->render();
}

} // namespace tui
