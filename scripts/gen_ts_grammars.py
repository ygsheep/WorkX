#!/usr/bin/env python3
"""gen_ts_grammars.py — 自动生成 tree-sitter grammar 注册代码

用途:
    维护一份集中的语言清单, 自动生成三处注册代码, 新增一种语言只需在
    GRAMMARS 里加一行后重新运行本脚本, 无需手改 CMake / C++。

生成产物:
    cmake/ts_grammars.cmake           workx_fetch_ts_grammar(...) 调用列表
    src/tui/render/ts_langs_decl.inc  extern "C" 声明 (tree_sitter_<name>)
    src/tui/render/ts_langs_reg.inc   GrammarRegistry 构造体注册块

前提:
    每个 grammar 仓库必须已 commit 预生成的 src/parser.c (+ 可能的 scanner),
    避免构建期需要 tree-sitter-cli 现场生成。仓库默认 grammar 源码在 src/,
    若在子目录 (如 typescript/src) 用 subdir 指定。

用法:
    python scripts/gen_ts_grammars.py           生成并覆写三个文件
    python scripts/gen_ts_grammars.py --list    仅打印语言清单 (不写文件)
    python scripts/gen_ts_grammars.py --check   校验已生成文件与清单一致
"""

import argparse
import sys
from pathlib import Path

# 脚本位于 scripts/, 项目根在上一级
ROOT = Path(__file__).resolve().parent.parent

# ============================================================================
# 语言清单 — 新增语言只改这里
# 字段: (name, repo, tag, subdir, [aliases...])
#   name     : grammar 名, 决定 tree_sitter_<name>() 入口 与 WORKX_TS_<NAME> 宏
#   repo     : git 仓库 URL
#   tag      : 固定 commit/tag (无 release tag 的仓库用分支 HEAD commit)
#   subdir   : grammar 源码子目录 (默认 "src"; typescript/tsx 用双子目录)
#   aliases  : 注册到语法高亮器的语言标签 (第一个为主标签)
# ============================================================================
GRAMMARS = [
    ("bash",		"https://github.com/tree-sitter/tree-sitter-bash",		"a06c2e4415e9bc0346c6b86d401879ffb44058f7",		"",		["bash", "sh", "shell", "zsh"]),
    ("c",		"https://github.com/tree-sitter/tree-sitter-c",		"b780e47fc780ddc8da13afa35a3f4ed5c157823d",		"",		["c"]),
    ("cpp",		"https://github.com/tree-sitter/tree-sitter-cpp",		"8b5b49eb196bec7040441bee33b2c9a4838d6967",		"",		["cpp", "c++", "cxx", "cc", "h", "hpp"]),
    ("go",		"https://github.com/tree-sitter/tree-sitter-go",		"v0.25.0",		"",		["go", "golang"]),
    ("javascript",		"https://github.com/tree-sitter/tree-sitter-javascript",		"v0.25.0",		"",		["javascript", "js", "jsx"]),
    ("json",		"https://github.com/tree-sitter/tree-sitter-json",		"254c42a6476413b776221e03982ac8ae159eeb72",		"",		["json"]),
    ("python",		"https://github.com/tree-sitter/tree-sitter-python",		"v0.25.0",		"",		["python", "py"]),
    ("rust",		"https://github.com/tree-sitter/tree-sitter-rust",		"v0.24.2",		"",		["rust", "rs"]),
    ("tsx",		"https://github.com/tree-sitter/tree-sitter-typescript",		"v0.23.2",		"tsx/src",		["tsx"]),
    ("typescript",		"https://github.com/tree-sitter/tree-sitter-typescript",		"v0.23.2",		"typescript/src",		["typescript", "ts"]),
]

# 生成产物路径
CMAKE_OUT = ROOT / "cmake" / "ts_grammars.cmake"
DECL_OUT = ROOT / "src" / "tui" / "render" / "ts_langs_decl.inc"
REG_OUT = ROOT / "src" / "tui" / "render" / "ts_langs_reg.inc"

HEADER = "# 本文件由 scripts/gen_ts_grammars.py 自动生成, 请勿手改. 改语言清单后重跑该脚本.\n"
INCLUDER = "// 本文件由 scripts/gen_ts_grammars.py 自动生成, 请勿手改. 改语言清单后重跑该脚本.\n"


def macro_name(name: str) -> str:
    """grammar 名 -> WORKX_TS_<NAME> 宏后缀 (连字符转下划线、大写)."""
    return name.replace("-", "_").upper()


def c_symbol(name: str) -> str:
    """grammar 名 -> C 入口符号后缀 (连字符转下划线)."""
    return name.replace("-", "_")


def gen_cmake() -> str:
    lines = [HEADER]
    lines.append("if(WORKX_HAS_TREE_SITTER AND WORKX_FETCH_GRAMMARS)")
    lines.append('    message(STATUS "Fetching tree-sitter grammars:")')
    for name, repo, tag, subdir, _ in GRAMMARS:
        args = f'{name:<14} "{repo}"  "{tag}"'
        if subdir:
            args += f' "{subdir}"'
        lines.append(f"    workx_fetch_ts_grammar({args})")
    lines.append("endif()")
    lines.append("")
    return "\n".join(lines)


def gen_decl() -> str:
    lines = [INCLUDER]
    for name, *_ in GRAMMARS:
        sym = c_symbol(name)
        macro = macro_name(name)
        lines.append(f"#ifdef WORKX_TS_{macro}")
        lines.append(f"    const TSLanguage* tree_sitter_{sym}(void);")
        lines.append("#endif")
    lines.append("")
    return "\n".join(lines)


def gen_reg() -> str:
    lines = [INCLUDER]
    for name, *_omit, aliases in [(g[0], g[1], g[2], g[3], g[4]) for g in GRAMMARS]:
        macro = macro_name(name)
        sym = c_symbol(name)
        lines.append(f"#ifdef WORKX_TS_{macro}")
        for alias in aliases:
            lines.append(f'            langs["{alias}"] = tree_sitter_{sym}();')
        lines.append("#endif")
    lines.append("")
    return "\n".join(lines)


def write_if_changed(path: Path, content: str) -> bool:
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists() and path.read_text(encoding="utf-8") == content:
        return False
    path.write_text(content, encoding="utf-8", newline="\n")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description="自动生成 tree-sitter grammar 注册代码")
    parser.add_argument("--list", action="store_true", help="仅打印语言清单, 不写文件")
    parser.add_argument("--check", action="store_true", help="校验已生成文件与清单一致")
    args = parser.parse_args()

    if args.list:
        print(f"共 {len(GRAMMARS)} 种语言:")
        for name, repo, tag, subdir, aliases in GRAMMARS:
            sub = f" ({subdir})" if subdir else ""
            print(f"  {name:<12} {repo.split('/')[-1]:<32} {tag:<26} aliases={aliases}{sub}")
        return 0

    outputs = [("cmake", CMAKE_OUT, gen_cmake()),
               ("decl", DECL_OUT, gen_decl()),
               ("reg", REG_OUT, gen_reg())]

    if args.check:
        ok = True
        for label, path, content in outputs:
            if path.exists() and path.read_text(encoding="utf-8") == content:
                print(f"  [OK] {path.relative_to(ROOT)}")
            else:
                print(f"  [STALE] {path.relative_to(ROOT)}")
                ok = False
        return 0 if ok else 1

    changed = False
    for label, path, content in outputs:
        if write_if_changed(path, content):
            print(f"  [write] {path.relative_to(ROOT)}")
            changed = True
        else:
            print(f"  [skip ] {path.relative_to(ROOT)} (unchanged)")
    if not changed:
        print("全部产物已是最新, 无需改动.")
    return 0


if __name__ == "__main__":
    sys.exit(main())