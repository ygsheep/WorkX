/**
 * @file test_input_processor.cpp
 * @brief InputProcessor + IFileLoader 单元测试（H-11：副作用隔离）
 * @details 验证 IFileLoader 接口注入后，InputProcessor 可在不接触真实文件系统的情况下测试。
 *          C-2：load 返回 std::optional<std::string>，区分"失败"与"空文件"。
 *          H-C：InputProcessor 构造函数无默认实参，调用方必须显式注入 IFileLoader。
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>

#include "agent/input/processor.h"
#include "agent/input/i_file_loader.h"
#include "agent/command/inclaude/registry.h"

using namespace agent::input;
using namespace agent::command;

namespace {

/// @brief 内存文件加载器（测试 mock）
/// @details 通过 path → content 映射模拟文件系统。未注册路径返回 std::nullopt
///          （模拟打开失败）。C-2：用 std::nullopt 表示失败，与空文件内容区分。
class InMemoryFileLoader final : public IFileLoader {
public:
    /// @brief 注册文件内容（空字符串表示合法的空文件）
    void set(const std::string& path, const std::string& content) {
        files_[path] = content;
    }

    std::optional<std::string> load(const std::string& path) override {
        auto it = files_.find(path);
        if (it == files_.end()) {
            return std::nullopt;  // 模拟打开失败
        }
        return it->second;  // C-2：返回 optional，空字符串也被视为合法内容
    }

private:
    std::unordered_map<std::string, std::string> files_;
};

} // namespace

// ============================================================
// IFileLoader 接口与 LocalFileLoader
// ============================================================

TEST_CASE("LocalFileLoader reads existing file", "[input][h11][file_loader]") {
    LocalFileLoader loader;
    // 用本测试文件自身验证（确定存在）
    auto content = loader.load(__FILE__);
    REQUIRE(content.has_value());
    REQUIRE_FALSE(content->empty());
    REQUIRE_THAT(*content, Catch::Matchers::ContainsSubstring("LocalFileLoader reads existing file"));
}

TEST_CASE("LocalFileLoader returns nullopt for nonexistent file", "[input][h11][file_loader][c2]") {
    LocalFileLoader loader;
    auto content = loader.load("/nonexistent/workx_test_path_h11.txt");
    REQUIRE_FALSE(content.has_value());  // C-2：失败返回 nullopt
}

// C-2：新增空文件测试，验证"失败"与"空文件"被正确区分
TEST_CASE("LocalFileLoader distinguishes empty file from missing file (C-2)", "[input][h11][file_loader][c2]") {
    // 用临时文件验证空文件场景
    namespace fs = std::filesystem;
    auto tmp_dir = fs::temp_directory_path() / "workx_test_c2";
    fs::create_directories(tmp_dir);
    auto empty_file = tmp_dir / "empty.txt";
    {
        std::ofstream ofs(empty_file, std::ios::binary);
        ofs.close();
    }
    // 临时清理，避免污染
    struct ScopeGuard {
        fs::path dir;
        ~ScopeGuard() { fs::remove_all(dir); }
    } guard{tmp_dir};

    LocalFileLoader loader;
    auto content = loader.load(empty_file.string());
    REQUIRE(content.has_value());      // 空文件：成功读取，返回 optional
    REQUIRE(content->empty());          // 内容为空字符串，但 has_value()=true

    auto missing = loader.load((tmp_dir / "not_exist.txt").string());
    REQUIRE_FALSE(missing.has_value()); // 失败：返回 nullopt
}

TEST_CASE("InMemoryFileLoader returns registered content", "[input][h11][file_loader]") {
    InMemoryFileLoader loader;
    loader.set("/fake/foo.txt", "hello world");
    auto content = loader.load("/fake/foo.txt");
    REQUIRE(content.has_value());
    REQUIRE(*content == "hello world");
}

TEST_CASE("InMemoryFileLoader returns nullopt for unregistered path", "[input][h11][file_loader][c2]") {
    InMemoryFileLoader loader;
    auto content = loader.load("/not/registered");
    REQUIRE_FALSE(content.has_value());  // C-2：失败返回 nullopt
}

// C-2：空文件场景的 mock 验证
TEST_CASE("InMemoryFileLoader distinguishes empty string from missing (C-2)", "[input][h11][file_loader][c2]") {
    InMemoryFileLoader loader;
    loader.set("/fake/empty.txt", "");  // 注册空字符串（合法空文件）
    auto content = loader.load("/fake/empty.txt");
    REQUIRE(content.has_value());   // 空文件：has_value()=true
    REQUIRE(content->empty());      // 但内容为空

    auto missing = loader.load("/not/registered");
    REQUIRE_FALSE(missing.has_value());  // 失败：has_value()=false
}

// ============================================================
// InputProcessor @file 注入逻辑（H-11 核心）
// ============================================================

TEST_CASE("InputProcessor injects file content via IFileLoader (H-11)", "[input][h11][processor]") {
    // 准备：注册一个 fake 文件
    auto loader = std::make_shared<InMemoryFileLoader>();
    loader->set("/path/to/foo.txt", "file content here");

    auto registry = std::make_shared<CommandRegistry>();
    InputProcessor processor(registry, loader);

    // 执行：用户输入带 @file 引用
    // 注意：InputParser 会把 @path 提取到 attachments，process_text_prompt 再读取
    auto result = processor.process("read this @/path/to/foo.txt", CommandContext{});

    // 验证：file_loader 被调用，文件内容出现在 messages
    REQUIRE(result.should_query);
    REQUIRE_FALSE(result.messages.empty());
    bool found_content = false;
    for (const auto& m : result.messages) {
        if (m.find("file content here") != std::string::npos) {
            found_content = true;
            break;
        }
    }
    REQUIRE(found_content);
}

TEST_CASE("InputProcessor handles file load failure gracefully (H-11)", "[input][h11][processor]") {
    // 准备：空 loader，所有 load 返回 nullopt
    auto loader = std::make_shared<InMemoryFileLoader>();
    auto registry = std::make_shared<CommandRegistry>();
    InputProcessor processor(registry, loader);

    // 执行：引用不存在的文件
    auto result = processor.process("read @/nonexistent/file.txt", CommandContext{});

    // 验证：不崩溃，错误提示出现在 messages
    REQUIRE(result.should_query);
    bool found_error = false;
    for (const auto& m : result.messages) {
        if (m.find("[Could not read file:") != std::string::npos) {
            found_error = true;
            break;
        }
    }
    REQUIRE(found_error);
}

// C-2：InputProcessor 正确处理空文件（不再误报为 "Could not read file"）
TEST_CASE("InputProcessor handles empty file correctly (C-2)", "[input][h11][processor][c2]") {
    auto loader = std::make_shared<InMemoryFileLoader>();
    loader->set("/fake/empty.txt", "");  // 空文件
    auto registry = std::make_shared<CommandRegistry>();
    InputProcessor processor(registry, loader);

    auto result = processor.process("read @/fake/empty.txt", CommandContext{});

    REQUIRE(result.should_query);
    bool found_file_tag = false;
    bool found_error = false;
    for (const auto& m : result.messages) {
        if (m.find("[Could not read file:") != std::string::npos) {
            found_error = true;
        }
        if (m.find("<file path=\"/fake/empty.txt\">") != std::string::npos) {
            found_file_tag = true;
        }
    }
    // 空文件应被正常处理为 <file> 标签，而非误报为读取失败
    REQUIRE(found_file_tag);
    REQUIRE_FALSE(found_error);
}

// H-C：验证 InputProcessor 构造函数无默认实参（编译期检查）
// 若恢复默认实参，下面的代码仍能编译；但本测试确保显式注入路径仍可用
TEST_CASE("InputProcessor requires explicit IFileLoader injection (H-C)", "[input][h11][processor][h-c]") {
    static_assert(!std::is_default_constructible_v<InputProcessor>,
                  "InputProcessor must not be default constructible");
    static_assert(!std::is_constructible_v<InputProcessor, std::shared_ptr<CommandRegistry>>,
                  "InputProcessor must require explicit IFileLoader (no default argument)");

    auto loader = std::make_shared<InMemoryFileLoader>();
    auto registry = std::make_shared<CommandRegistry>();
    InputProcessor processor(registry, loader);  // 显式注入
    (void)processor;
}

// ============================================================
// 图片附件：@path → image_paths（绝对路径）
// ============================================================

TEST_CASE("InputProcessor converts image ref to absolute path", "[input][processor][vision]") {
    // 真实临时图片文件（仅扩展名有意义，processor 不做内容校验）
    namespace fs = std::filesystem;
    auto img_path = fs::temp_directory_path() / "workx_input_test_img.png";
    {
        std::ofstream ofs(img_path, std::ios::binary);
        ofs << "fake-image-bytes";
    }
    struct ScopeGuard {
        fs::path p;
        ~ScopeGuard() { fs::remove(p); }
    } guard{img_path};

    auto loader = std::make_shared<InMemoryFileLoader>();
    auto registry = std::make_shared<CommandRegistry>();
    InputProcessor processor(registry, loader);

    // 带引号的绝对路径：@"<abs>"；parser 提取后 processor 校验存在并保留绝对路径
    auto result = processor.process("描述这张图 @\"" + img_path.string() + "\"", CommandContext{});

    REQUIRE(result.should_query);
    REQUIRE(result.image_paths.size() == 1);
    REQUIRE(result.image_paths[0] == fs::weakly_canonical(img_path).string());
    // 文本保留在 messages
    bool found_text = false;
    for (const auto& m : result.messages) {
        if (m.find("描述这张图") != std::string::npos) found_text = true;
    }
    REQUIRE(found_text);
}

TEST_CASE("InputProcessor reports missing image and skips it", "[input][processor][vision]") {
    auto loader = std::make_shared<InMemoryFileLoader>();
    auto registry = std::make_shared<CommandRegistry>();
    InputProcessor processor(registry, loader);

    auto result = processor.process("看图 @/nonexistent/workx_missing.png", CommandContext{});

    REQUIRE(result.should_query);
    REQUIRE(result.image_paths.empty());
    bool found_error = false;
    for (const auto& m : result.messages) {
        if (m.find("[Could not read image:") != std::string::npos) found_error = true;
    }
    REQUIRE(found_error);
}

// ============================================================
// 多命令支持："/skill1 + /skill2" 逐个执行并合并结果
// ============================================================

namespace {

/// @brief 注册一个 PromptCommand（模拟 skill），提示文本为固定字符串
std::shared_ptr<PromptCommand> make_skill_cmd(const std::string& name,
                                              const std::string& prompt) {
    auto cmd = make_prompt_command(name, "skill " + name);
    cmd->set_prompt_generator([prompt](const std::string&,
                                       const CommandContext&) {
        return std::vector<PromptBlock>{PromptBlock{
            .type = PromptBlockType::Text, .text = prompt, .image = {}}};
    });
    return cmd;
}

} // namespace

TEST_CASE("InputProcessor executes multiple slash commands joined by +",
          "[input][processor][multi_command]") {
    auto loader = std::make_shared<InMemoryFileLoader>();
    auto registry = std::make_shared<CommandRegistry>();
    registry->register_command(make_skill_cmd("skill-001", "PROMPT_A"));
    registry->register_command(make_skill_cmd("skill-002", "PROMPT_B"));
    InputProcessor processor(registry, loader);

    auto result = processor.process("/skill-001 + /skill-002", CommandContext{});

    REQUIRE(result.should_query);
    REQUIRE_FALSE(result.is_error);
    // 两个 skill 的提示文本都合并进 output_text
    REQUIRE(result.output_text.find("PROMPT_A") != std::string::npos);
    REQUIRE(result.output_text.find("PROMPT_B") != std::string::npos);
    REQUIRE(result.output_text.find("PROMPT_A") < result.output_text.find("PROMPT_B"));
}

TEST_CASE("InputProcessor single slash command unaffected by + splitting",
          "[input][processor][multi_command]") {
    auto loader = std::make_shared<InMemoryFileLoader>();
    auto registry = std::make_shared<CommandRegistry>();
    registry->register_command(make_skill_cmd("skill-001", "PROMPT_A"));
    InputProcessor processor(registry, loader);

    auto result = processor.process("/skill-001", CommandContext{});

    REQUIRE(result.should_query);
    REQUIRE(result.output_text.find("PROMPT_A") != std::string::npos);
}

TEST_CASE("InputProcessor treats + inside args as non-multi command",
          "[input][processor][multi_command]") {
    auto loader = std::make_shared<InMemoryFileLoader>();
    auto registry = std::make_shared<CommandRegistry>();
    // 记录被调用的命令名
    std::vector<std::string> called;
    auto cmd = make_prompt_command("skill-001", "skill");
    cmd->set_prompt_generator([&called](const std::string& args,
                                        const CommandContext&) {
        called.push_back(args);
        return std::vector<PromptBlock>{PromptBlock{
            .type = PromptBlockType::Text, .text = "PROMPT", .image = {}}};
    });
    registry->register_command(cmd);
    InputProcessor processor(registry, loader);

    // "+ bar" 不是 "/" 开头 → 不按多命令拆分，整串作为单命令参数
    auto result = processor.process("/skill-001 + bar", CommandContext{});

    REQUIRE(result.should_query);
    REQUIRE(called.size() == 1);
    REQUIRE(called[0] == "+ bar");
}

TEST_CASE("InputProcessor multi command reports error for missing command",
          "[input][processor][multi_command]") {
    auto loader = std::make_shared<InMemoryFileLoader>();
    auto registry = std::make_shared<CommandRegistry>();
    registry->register_command(make_skill_cmd("skill-001", "PROMPT_A"));
    InputProcessor processor(registry, loader);

    auto result = processor.process("/skill-001 + /nope", CommandContext{});

    // 存在的命令正常执行，缺失命令报错
    REQUIRE(result.is_error);
    REQUIRE(result.output_text.find("PROMPT_A") != std::string::npos);
    REQUIRE(result.output_text.find("Command not found") != std::string::npos);
}
