/**
 * @file test_skill_loader.cpp
 * @brief Skill 磁盘加载器单元测试
 * @details 使用临时目录构造 .claude/skills 结构，验证扫描/解析/去重/别名
 */

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <random>

#include "agent/skill/inclaude/skill_loader.h"

namespace fs = std::filesystem;
using namespace agent::skill;
using namespace agent::command;

namespace {

/// @brief 临时目录 RAII（同工具测试惯例）
class TempDir {
public:
    TempDir() {
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<int> dist(100000, 999999);
        path_ = fs::temp_directory_path() / ("workx_skill_test_" + std::to_string(dist(gen)));
        fs::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    fs::path make_file(const fs::path& rel, const std::string& content) const {
        auto fp = path_ / rel;
        fs::create_directories(fp.parent_path());
        std::ofstream out(fp, std::ios::binary);
        out << content;
        out.close();
        return fp;
    }
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

const std::string kSample =
    "---\n"
    "name: sample\n"
    "description: A sample skill\n"
    "aliases: s, smpl\n"
    "---\n"
    "# Sample\n"
    "Do the thing\n";

} // anonymous namespace

TEST_CASE("load a valid skill from dir", "[skill][loader]") {
    TempDir tmp;
    tmp.make_file("proj/.claude/skills/sample/SKILL.md", kSample);
    const auto dir = (tmp.path() / "proj/.claude/skills").string();

    const auto cmds = load_skills_from_dirs({dir});

    REQUIRE(cmds.size() == 3);  // 本体 + 2 个别名
    REQUIRE(cmds[0]->name() == "sample");
    REQUIRE(cmds[0]->description() == "A sample skill");
    REQUIRE(cmds[0]->loaded_from() == LoadSource::Skills);
    REQUIRE(cmds[0]->source() == "skills");
    REQUIRE(cmds[0]->is_user_invocable());
    REQUIRE_FALSE(cmds[0]->is_model_invocation_disabled());
    REQUIRE(cmds[0]->type() == "prompt");
}

TEST_CASE("aliases become separate commands sharing the prompt", "[skill][loader]") {
    TempDir tmp;
    tmp.make_file("proj/.claude/skills/sample/SKILL.md", kSample);
    const auto dir = (tmp.path() / "proj/.claude/skills").string();

    const auto cmds = load_skills_from_dirs({dir});

    REQUIRE(cmds.size() == 3);
    REQUIRE(cmds[1]->name() == "s");
    REQUIRE(cmds[2]->name() == "smpl");
    REQUIRE(cmds[1]->loaded_from() == LoadSource::Skills);
}

TEST_CASE("generator emits baseDir prefix plus body", "[skill][loader]") {
    TempDir tmp;
    tmp.make_file("proj/.claude/skills/sample/SKILL.md", kSample);
    const auto dir = (tmp.path() / "proj/.claude/skills").string();

    const auto cmds = load_skills_from_dirs({dir});
    REQUIRE(cmds.size() == 3);

    const auto blocks = cmds[0]->generate_prompt("", CommandContext{});
    REQUIRE(blocks.size() == 1);
    REQUIRE(blocks[0].type == PromptBlockType::Text);
    REQUIRE(blocks[0].text.find("Base directory for this skill:") != std::string::npos);
    REQUIRE(blocks[0].text.find("# Sample") != std::string::npos);
}

TEST_CASE("dirs without SKILL.md are skipped", "[skill][loader]") {
    TempDir tmp;
    tmp.make_file("proj/.claude/skills/sample/SKILL.md", kSample);
    tmp.make_file("proj/.claude/skills/no-skill/notes.txt", "not a skill");
    const auto dir = (tmp.path() / "proj/.claude/skills").string();

    const auto cmds = load_skills_from_dirs({dir});

    REQUIRE(cmds.size() == 3);
    for (const auto& cmd : cmds) {
        REQUIRE(cmd->name() != "no-skill");
    }
}

TEST_CASE("name falls back to directory name", "[skill][loader]") {
    TempDir tmp;
    tmp.make_file("proj/.claude/skills/fallback/SKILL.md",
                  "---\n"
                  "description: no name field\n"
                  "---\n"
                  "body\n");
    const auto dir = (tmp.path() / "proj/.claude/skills").string();

    const auto cmds = load_skills_from_dirs({dir});

    REQUIRE(cmds.size() == 1);
    REQUIRE(cmds[0]->name() == "fallback");
}

TEST_CASE("user_invocable false is honored", "[skill][loader]") {
    TempDir tmp;
    tmp.make_file("proj/.claude/skills/private/SKILL.md",
                  "---\n"
                  "user_invocable: false\n"
                  "---\n");
    const auto dir = (tmp.path() / "proj/.claude/skills").string();

    const auto cmds = load_skills_from_dirs({dir});

    REQUIRE(cmds.size() == 1);
    REQUIRE_FALSE(cmds[0]->is_user_invocable());
}

TEST_CASE("same file listed from two dirs is deduplicated", "[skill][loader]") {
    TempDir tmp;
    const auto file = tmp.make_file("proj/.claude/skills/sample/SKILL.md", kSample);
    const auto dir_a = (tmp.path() / "proj/.claude/skills").string();
    // 通过符号链接制造同一文件的第二个入口（Windows 需管理员权限时用复制路径模拟）
    std::error_code ec;
    fs::create_directory_symlink(file.parent_path(), tmp.path() / "alias_dir", ec);
    std::vector<std::string> dirs{dir_a};
    if (!ec) {
        dirs.push_back((tmp.path() / "alias_dir").string());
    }

    const auto cmds = load_skills_from_dirs(dirs);

    REQUIRE(cmds.size() == 3);
}

TEST_CASE("find_skill_dirs_up_to_home returns deepest first", "[skill][loader]") {
    TempDir tmp;
    tmp.make_file("a/.claude/skills/one/SKILL.md", kSample);
    tmp.make_file("a/b/.claude/skills/two/SKILL.md", kSample);
    const auto cwd = (tmp.path() / "a" / "b").string();

    const auto dirs = find_skill_dirs_up_to_home(cwd);

    REQUIRE(dirs.size() >= 2);
    REQUIRE(dirs[0] == (fs::path(cwd) / ".claude" / "skills").string());
    REQUIRE(dirs[1] == (tmp.path() / "a" / ".claude" / "skills").string());
}
