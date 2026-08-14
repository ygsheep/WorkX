/**
 * @file main.cpp
 * @brief 语法高亮渲染示例
 * @details 直接调用 highlight_code 渲染各种语言代码 + diff,
 *          验证 tree-sitter 集成效果。同时模拟 FileRead 行号场景。
 *
 * 渲染路径覆盖:
 *   1. 多语言代码块 (cpp/python/rust/go/bash/json/javascript)
 *   2. diff 渲染 (addition/deletion/context/hunk_header)
 *   3. FileRead 模拟 (带行号 → 剥离 → 高亮 → 加回行号)
 *   4. 错误恢复 (LLM 半截代码, 括号没闭合)
 *   5. 未知语言降级 (原样输出)
 */

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#endif

#include "tui/render/syntax_highlighter.h"
#include "tui/render/markdown_renderer.h"
#include "tui/utils/utf8_utils.h"

using namespace tui;

// ============================================================================
// ANSI / Box-drawing 辅助
// ============================================================================

namespace ansi {
    constexpr auto RESET = "\x1b[0m";
    constexpr auto DIM   = "\x1b[2m";
    constexpr auto GRAY  = "\x1b[90m";
}

namespace box {
    constexpr auto TL = "\xe2\x94\x8c";  // ┌
    constexpr auto TR = "\xe2\x94\x90";  // ┐
    constexpr auto BL = "\xe2\x94\x94";  // └
    constexpr auto BR = "\xe2\x94\x98";  // ┘
    constexpr auto H  = "\xe2\x94\x80";  // ─
    constexpr auto V  = "\xe2\x94\x82";  // │
}

// ============================================================================
// 渲染辅助函数
// ============================================================================

/// 打印章节标题
static void print_section(const std::string& title) {
    std::cout << "\n" << ansi::DIM << "══ " << title << " ══" << ansi::RESET << "\n\n";
}

/// 统一渲染格式: |N  内容 (无右边框)
/// @param lang  语言标签 (用于 highlight_code)
/// @param code  原始代码
static void render_code(const std::string& lang, std::string_view code) {
    // 对 diff 走 highlight_diff, 用 cpp 作为文件语言 (示例: cpp 文件的 diff)
    std::string highlighted = (lang == "diff")
        ? highlight_diff("cpp", code)
        : highlight_code(lang, code);

    // 拆行
    std::vector<std::string> lines;
    {
        std::string cur;
        for (char c : highlighted) {
            if (c == '\n') { lines.push_back(std::move(cur)); cur.clear(); }
            else cur.push_back(c);
        }
        if (!cur.empty()) lines.push_back(std::move(cur));
    }

    if (lines.empty()) return;

    // 行号宽度 (右对齐)
    int width = 1;
    int last = static_cast<int>(lines.size());
    for (int n = last; n >= 10; n /= 10) ++width;

    // 输出: │N  内容 (无右边框)
    // 用 box-drawing 字符 │ (U+2502) 代替 |, 终端中上下行无缝衔接
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string num = std::to_string(i + 1);
        std::cout << ansi::GRAY << box::V << ansi::RESET;
        // 行号右对齐
        if (static_cast<int>(num.size()) < width) {
            for (int j = 0; j < width - static_cast<int>(num.size()); ++j)
                std::cout << " ";
        }
        std::cout << ansi::DIM << num << ansi::RESET;
        std::cout << "  ";  // 行号和内容之间两个空格
        std::cout << lines[i];
        std::cout << ansi::RESET << "\n";
    }
}

// ============================================================================
// 测试用例
// ============================================================================

static const char* CPP_CODE = R"(
#include <iostream>
#include <vector>
#include <string>

// 简单的图形渲染器
class Renderer {
public:
    Renderer(int width, int height) : width_(width), height_(height) {}

    void draw(const std::string& text) {
        for (int i = 0; i < height_; ++i) {
            std::cout << text << "\n";
        }
    }

private:
    int width_;
    int height_;
};

int main() {
    Renderer r(80, 24);
    r.draw("Hello, World!");
    return 0;
}
)";

static const char* PYTHON_CODE = R"(
import os
from typing import List

def fibonacci(n: int) -> List[int]:
    """生成斐波那契数列"""
    if n <= 0:
        return []
    elif n == 1:
        return [0]

    result = [0, 1]
    for i in range(2, n):
        result.append(result[i-1] + result[i-2])
    return result

# 主入口
if __name__ == "__main__":
    print(fibonacci(10))
)";

static const char* RUST_CODE = R"(
use std::collections::HashMap;

fn main() {
    let mut scores: HashMap<String, i32> = HashMap::new();

    scores.insert(String::from("Alice"), 10);
    scores.insert(String::from("Bob"), 20);

    for (name, score) in &scores {
        println!("{}: {}", name, score);
    }

    match scores.get("Alice") {
        Some(&s) => println!("Alice's score: {}", s),
        None => println!("Alice not found"),
    }
}
)";

static const char* GO_CODE = R"GO(
package main

import (
    "fmt"
    "strings"
)

func main() {
    names := []string{"Alice", "Bob", "Charlie"}

    for i, name := range names {
        greeting := fmt.Sprintf("Hello, %s! (index: %d)", name, i)
        fmt.Println(strings.ToUpper(greeting))
    }
}
)GO";

static const char* BASH_CODE = R"BASH(#!/bin/bash
# 部署脚本示例
set -e

PROJECT_DIR="/opt/myapp"
BACKUP_DIR="${PROJECT_DIR}/backup"

if [ ! -d "$PROJECT_DIR" ]; then
    echo "Project directory not found"
    exit 1
fi

for file in *.sh; do
    if [ -f "$file" ]; then
        cp "$file" "$BACKUP_DIR/"
        echo "Backed up: $file"
    fi
done
)BASH";

static const char* JSON_CODE = R"JSON({
    "name": "workx",
    "version": "0.2.0",
    "description": "Terminal AI chat client",
    "dependencies": [
        "nlohmann-json",
        "curl",
        "tree-sitter"
    ],
    "features": {
        "syntax_highlight": true,
        "streaming": true
    }
})JSON";

static const char* JS_CODE = R"JS(
class EventBus {
    constructor() {
        this.handlers = new Map();
    }

    on(event, callback) {
        if (!this.handlers.has(event)) {
            this.handlers.set(event, []);
        }
        this.handlers.get(event).push(callback);
    }

    emit(event, data) {
        const callbacks = this.handlers.get(event) || [];
        callbacks.forEach(cb => cb(data));
    }
}

const bus = new EventBus();
bus.on('token', (text) => console.log(text));
)JS";

static const char* TS_CODE = R"TS(
interface Config {
    name: string;
    timeout?: number;
}

async function loadConfig(path: string): Promise<Config> {
    const raw = await fetch(path);
    if (!raw.ok) throw new Error(`HTTP ${raw.status}`);
    const cfg: Config = await raw.json();
    return cfg.timeout ? cfg : { ...cfg, timeout: 3000 };
}

const config = await loadConfig("./config.json");
console.log(config.name);
)TS";

static const char* TSX_CODE = R"TSX(
import React, { useState } from "react";

export function Counter({ initial = 0 }: { initial?: number }) {
    const [count, setCount] = useState(initial);
    return (
        <button onClick={() => setCount(count + 1)}>
            Count: {count}
        </button>
    );
}
)TSX";

static const char* NIX_CODE = R"NIX(
{ pkgs, lib, ... }:

let
  name = "workx";
in
pkgs.stdenv.mkDerivation {
  inherit name;
  src = ./.;
  nativeBuildInputs = with pkgs; [ cmake ninja ];
  buildPhase = ''
    cmake -B build -DCMAKE_BUILD_TYPE=Release
    cmake --build build
  '';
}
)NIX";

static const char* HTML_CODE = R"HTML(
<!DOCTYPE html>
<html lang="zh">
<head>
    <meta charset="utf-8">
    <title>workx</title>
    <style>
        body { margin: 0; background: #1e1e2e; }
        .main { display: flex; }
    </style>
</head>
<body>
    <div class="main">
        <h1>Hello, workx!</h1>
        <script>
            const el = document.querySelector("h1");
            el.textContent = "你好";
        </script>
    </div>
</body>
</html>
)HTML";

static const char* YAML_CODE = R"YAML(
version: "3.9"
services:
  app:
    build: .
    ports:
      - "8080:8080"
    environment:
      - WORKX_LOG_LEVEL=info
    volumes:
      - ./data:/var/lib/workx
)YAML";

static const char* TOML_CODE = R"TOML(
[build]
cmake_preset = "default"
tests = true

[tool.workx]
log_level = "info"
audit = { enabled = true, retention_days = 30 }
)TOML";

static const char* JAVA_CODE = R"JAVA(
import java.util.List;

public class Main {
    public static void main(String[] args) {
        List<String> names = List.of("workx", "tui");
        names.stream()
             .filter(n -> n.length() > 3)
             .forEach(System.out::println);
    }
}
)JAVA";

static const char* LUA_CODE = R"LUA(
local M = {}

function M.greet(name)
    local msg = "hello, " .. name
    print(msg)
    return #msg
end

return M
)LUA";

static const char* DIFF_CODE = R"DIFF(--- a/src/main.cpp
+++ b/src/main.cpp
@@ -5,7 +5,9 @@
 #include <string>
 #include <vector>

-int main() {
+int main(int argc, char* argv[]) {
+    if (argc < 2) return 1;
+
     std::vector<std::string> args;
-    for (int i = 0; i < 10; ++i) {
+    for (int i = 1; i < argc; ++i) {
         args.push_back(argv[i]);
     }
     return 0;
)DIFF";

/// 模拟 LLM 流式半截代码 (括号没闭合, 缺分号) — 验证 error recovery
static const char* INCOMPLETE_CPP = R"(
#include <iostream>

int main() {
    std::cout << "Hello, World!" << std::endl
    // 注意: 没有分号, 也没有 return 和闭合 }

    if (true) {
        for (int i = 0; i < 10; ++i) {
            std::cout << i)";

static const char* UNKNOWN_LANG_CODE = R"(
This is some pseudo-language code
that has no registered grammar.
It should fall through to original output
without any ANSI color codes.
)";

// ============================================================================
// 主函数
// ============================================================================

int main() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif

    std::cout << "Tree-sitter 语法高亮渲染测试\n";
    std::cout << "启用状态: " << (syntax_highlighting_enabled() ? "YES" : "NO") << "\n";

    // ====== 1. 多语言代码块 ======
    print_section("1. C++ 代码");
    render_code("cpp", CPP_CODE);

    print_section("2. Python 代码");
    render_code("python", PYTHON_CODE);

    print_section("3. Rust 代码");
    render_code("rust", RUST_CODE);

    print_section("4. Go 代码");
    render_code("go", GO_CODE);

    print_section("5. Bash 代码");
    render_code("bash", BASH_CODE);

    print_section("6. JSON 数据");
    render_code("json", JSON_CODE);

    print_section("7. JavaScript 代码");
    render_code("javascript", JS_CODE);

    print_section("7b. TypeScript 代码");
    render_code("typescript", TS_CODE);

    print_section("7c. TSX (React) 代码");
    render_code("tsx", TSX_CODE);

    print_section("7d. Nix 代码");
    render_code("nix", NIX_CODE);

    print_section("7e. HTML 代码");
    render_code("html", HTML_CODE);

    print_section("7f. YAML 数据");
    render_code("yaml", YAML_CODE);

    print_section("7g. TOML 配置");
    render_code("toml", TOML_CODE);

    print_section("7h. Java 代码");
    render_code("java", JAVA_CODE);

    print_section("7i. Lua 代码");
    render_code("lua", LUA_CODE);

    // ====== 2. diff 渲染 ======
    print_section("8. Diff (Write/Edit 工具返回, +/- 前缀不高亮, 内容整行高亮)");
    render_code("diff", DIFF_CODE);

    // ====== 3. FileRead 模拟 (统一格式, 行号由 render_code 生成) ======
    print_section("9. FileRead 模拟 (代码高亮)");
    {
        std::string code = "#include <iostream>\n\nint main() {\n"
            "    std::cout << \"Hello, World!\" << std::endl;\n"
            "    return 0;\n}\n";
        render_code("cpp", code);
    }

    // ====== 4. 错误恢复 (LLM 半截代码) ======
    print_section("10. 错误恢复 (LLM 半截代码, 括号没闭合)");
    render_code("cpp", INCOMPLETE_CPP);

    // ====== 5. 未知语言降级 ======
    print_section("11. 未知语言降级 (原样输出, 无 ANSI)");
    render_code("unknown-lang", UNKNOWN_LANG_CODE);

    std::cout << "\n";
    return 0;
}
