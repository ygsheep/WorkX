/**
 * @file test_input_processor.cpp
 * @brief InputProcessor + IFileLoader 单元测试（H-11：副作用隔离）
 * @details 验证 IFileLoader 接口注入后，InputProcessor 可在不接触真实文件系统的情况下测试。
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <memory>
#include <string>
#include <unordered_map>

#include "agent/input/processor.h"
#include "agent/input/i_file_loader.h"
#include "agent/command/inclaude/registry.h"

using namespace agent::input;
using namespace agent::command;

namespace {

/// @brief 内存文件加载器（测试 mock）
/// @details 通过 path → content 映射模拟文件系统。未注册路径返回空（模拟打开失败）。
class InMemoryFileLoader final : public IFileLoader {
public:
    /// @brief 注册文件内容
    void set(const std::string& path, const std::string& content) {
        files_[path] = content;
    }

    std::string load(const std::string& path) override {
        auto it = files_.find(path);
        if (it == files_.end()) {
            return {};  // 模拟打开失败
        }
        return it->second;
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
    std::string content = loader.load(__FILE__);
    REQUIRE_FALSE(content.empty());
    REQUIRE_THAT(content, Catch::Matchers::ContainsSubstring("LocalFileLoader reads existing file"));
}

TEST_CASE("LocalFileLoader returns empty for nonexistent file", "[input][h11][file_loader]") {
    LocalFileLoader loader;
    std::string content = loader.load("/nonexistent/workx_test_path_h11.txt");
    REQUIRE(content.empty());
}

TEST_CASE("InMemoryFileLoader returns registered content", "[input][h11][file_loader]") {
    InMemoryFileLoader loader;
    loader.set("/fake/foo.txt", "hello world");
    REQUIRE(loader.load("/fake/foo.txt") == "hello world");
}

TEST_CASE("InMemoryFileLoader returns empty for unregistered path", "[input][h11][file_loader]") {
    InMemoryFileLoader loader;
    REQUIRE(loader.load("/not/registered").empty());
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
    // 准备：空 loader，所有 load 返回空
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

TEST_CASE("InputProcessor default uses LocalFileLoader (H-11)", "[input][h11][processor]") {
    // 验证：不传 file_loader 时默认使用 LocalFileLoader
    auto registry = std::make_shared<CommandRegistry>();
    InputProcessor processor(registry);  // 不传 loader

    // 引用本测试文件（真实存在）
    std::string self = __FILE__;
    std::string input = "read @" + self;
    auto result = processor.process(input, CommandContext{});

    REQUIRE(result.should_query);
    bool found_self = false;
    for (const auto& m : result.messages) {
        if (m.find("InputProcessor default uses LocalFileLoader") != std::string::npos) {
            found_self = true;
            break;
        }
    }
    REQUIRE(found_self);
}
