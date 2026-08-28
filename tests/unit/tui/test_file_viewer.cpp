/**
 * @file test_file_viewer.cpp
 * @brief 文件查看器（/view 只读）无头渲染测试
 * @details 覆盖：空状态占位、路径栏（路径/行数/语言）、行号列、虚拟化滚动切片、
 *          语言推断（lang_from_path）。
 * @note 测试名用英文：Windows 上 CMake catch_discover_tests 对 GBK 管道
 *       捕获 UTF-8 中文名会损坏 JSON（既有环境行为）。
 */

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <ftxui/screen/screen.hpp>
#include <ftxui/screen/terminal.hpp>

#include "render/markdown_to_elements.h"
#include "widgets/file_viewer.h"

using namespace ftxtui;

namespace {

/// @brief 把元素渲染到固定尺寸 Screen 并返回文本
std::string render_elem(const ftxui::Element& e, int cols = 30, int rows = 24) {
    auto screen = ftxui::Screen::Create(ftxui::Dimension::Fixed(cols),
                                        ftxui::Dimension::Fixed(rows));
    ftxui::Render(screen, e);
    std::string out;
    for (int y = 0; y < rows; ++y) {
        for (int x = 0; x < cols; ++x) out += screen.PixelAt(x, y).character;
        out += '\n';
    }
    return out;
}

FileViewState make_file(std::vector<std::string> lines,
                        std::string path = "src/main.cpp",
                        std::string lang = "cpp",
                        int scroll = 0) {
    FileViewState f;
    f.path = std::move(path);
    f.lines = std::move(lines);
    f.lang = std::move(lang);
    f.scroll = scroll;
    return f;
}

}  // namespace

TEST_CASE("file viewer empty state shows placeholder", "[file_viewer][render]") {
    FileViewState f;  // path 为空
    const auto text = render_elem(build_file_viewer(f));
    REQUIRE(text.find("暂无打开的文件") != std::string::npos);
}

TEST_CASE("file viewer shows path bar with line count and lang", "[file_viewer][render]") {
    FileViewState f = make_file({"int main() {", "  return 0;", "}"});
    const auto text = render_elem(build_file_viewer(f), 40, 24);
    INFO("RENDERED:\n" << text);
    REQUIRE(text.find("src/main.cpp") != std::string::npos);
    REQUIRE(text.find("3 行") != std::string::npos);
    REQUIRE(text.find("cpp") != std::string::npos);
}

TEST_CASE("file viewer shows line numbers and content", "[file_viewer][render]") {
    FileViewState f = make_file({"int main() {", "  return 0;", "}"});
    const auto text = render_elem(build_file_viewer(f), 40, 24);
    REQUIRE(text.find("1") != std::string::npos);
    REQUIRE(text.find("2") != std::string::npos);
    REQUIRE(text.find("3") != std::string::npos);
    REQUIRE(text.find("int main() {") != std::string::npos);
    REQUIRE(text.find("return 0") != std::string::npos);
}

TEST_CASE("file viewer scrolls to offset slice", "[file_viewer][render]") {
    FileViewState f = make_file({"line0", "line1", "line2", "line3", "line4", "line5"},
                                "f.txt", "", 3);
    // 显式视口高度 3 行（绕过终端探测，保证 scroll=3 → line0/1/2 出视口）
    const auto text = render_elem(build_file_viewer(f, 40, 3), 40, 24);
    INFO("RENDERED:\n" << text);
    // scroll=3：从 line3 开始显示，line0/1/2 不可见
    REQUIRE(text.find("line3") != std::string::npos);
    REQUIRE(text.find("line4") != std::string::npos);
    REQUIRE(text.find("line0") == std::string::npos);
    REQUIRE(text.find("line1") == std::string::npos);
    REQUIRE(text.find("line2") == std::string::npos);
}

TEST_CASE("file viewer shows scroll hint", "[file_viewer][render]") {
    FileViewState f = make_file({"a"});
    const auto text = render_elem(build_file_viewer(f));
    REQUIRE(text.find("Esc 关闭") != std::string::npos);
}

TEST_CASE("file viewer line numbers right aligned", "[file_viewer][render]") {
    // 10 行 → 行号 2 位：1-9 前补空格，10 不补
    std::vector<std::string> lines;
    for (int i = 0; i < 10; ++i) lines.push_back("x" + std::to_string(i));
    FileViewState f = make_file(std::move(lines), "f.txt", "", 0);
    const auto text = render_elem(build_file_viewer(f), 40, 24);
    // 行号 10 存在且紧邻内容（无前导空格），行号 1 有前导空格
    REQUIRE(text.find("10 x9") != std::string::npos);
    REQUIRE(text.find(" 1 x0") != std::string::npos);
}

TEST_CASE("file viewer lang_from_path infers language", "[file_viewer][lang]") {
    REQUIRE(lang_from_path("src/main.cpp") == "cpp");
    REQUIRE(lang_from_path("src/main.py") == "python");
    REQUIRE(lang_from_path("CMakeLists.txt") == "cpp");
    REQUIRE(lang_from_path("Dockerfile") == "bash");
    REQUIRE(lang_from_path("README.md") == "");
    REQUIRE(lang_from_path("noext") == "");
    REQUIRE(lang_from_path("") == "");
}

// ============================================================================
// 内联 diff 高亮（P4）
// ============================================================================

TEST_CASE("file viewer marks inserted line with + marker", "[file_viewer][diff]") {
    FileViewState f = make_file({"int main() {", "  auto y = bar();", "  return 0;", "}"});
    // 修改区块：第 2 行为新增（new_start=2，diff 第 1 行 Insert）
    FileChange ch;
    ch.file_path = "src/main.cpp";
    ch.new_start = 2;
    ch.diff.push_back({agent::DiffKind::Insert, "  auto y = bar();", 1});
    f.changes.push_back(std::move(ch));

    const auto text = render_elem(build_file_viewer(f), 40, 24);
    INFO("RENDERED:\n" << text);
    // 第 2 行出现 + 标记；未变更行无 +
    REQUIRE(text.find("+2") != std::string::npos);
    REQUIRE(text.find("+1") == std::string::npos);
    REQUIRE(text.find("+3") == std::string::npos);
}

TEST_CASE("file viewer marks modified line with + marker", "[file_viewer][diff]") {
    FileViewState f = make_file({"int main() {", "  auto y = bar();", "  return 0;", "}"});
    // 修改区块：第 2 行为 Modify（new_start=2）
    FileChange ch;
    ch.file_path = "src/main.cpp";
    ch.new_start = 2;
    ch.diff.push_back({agent::DiffKind::Modify, "  auto y = bar();", 1});
    f.changes.push_back(std::move(ch));

    const auto text = render_elem(build_file_viewer(f), 40, 24);
    INFO("RENDERED:\n" << text);
    REQUIRE(text.find("+2") != std::string::npos);
}

TEST_CASE("file viewer diff with multi-line block marks each changed line", "[file_viewer][diff]") {
    FileViewState f = make_file({"a", "b", "c", "d"});
    FileChange ch;
    ch.file_path = "f.txt";
    ch.new_start = 2;
    ch.diff.push_back({agent::DiffKind::Equal, "b", 1});
    ch.diff.push_back({agent::DiffKind::Insert, "b1", 2});
    ch.diff.push_back({agent::DiffKind::Modify, "c1", 3});
    f.changes.push_back(std::move(ch));

    const auto text = render_elem(build_file_viewer(f), 40, 24);
    INFO("RENDERED:\n" << text);
    // 第 2 行 Equal 无 +，第 3 行 Insert 有 +，第 4 行 Modify 有 +
    REQUIRE(text.find("+2") == std::string::npos);
    REQUIRE(text.find("+3") != std::string::npos);
    REQUIRE(text.find("+4") != std::string::npos);
}

TEST_CASE("file viewer ignores change without new_start", "[file_viewer][diff]") {
    FileViewState f = make_file({"a", "b"});
    FileChange ch;
    ch.file_path = "f.txt";
    ch.new_start = 0;  // 未定位（找不到区块）
    ch.diff.push_back({agent::DiffKind::Insert, "b", 1});
    f.changes.push_back(std::move(ch));

    const auto text = render_elem(build_file_viewer(f), 40, 24);
    REQUIRE(text.find("+") == std::string::npos);
}
