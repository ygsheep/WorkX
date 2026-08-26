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
    ("bash",		"https://github.com/tree-sitter/tree-sitter-bash",		"a06c2e4415e9bc0346c6b86d401879ffb44058f7",		"",		["bash"]),
    ("c",		"https://github.com/tree-sitter/tree-sitter-c",		"b780e47fc780ddc8da13afa35a3f4ed5c157823d",		"",		["c"]),
    ("c-sharp",		"https://github.com/tree-sitter/tree-sitter-c-sharp",		"9150f7d56bb47f1a809fa23623f1ba1413e93fa9",		"",		["c-sharp"]),
    ("cmake",		"https://github.com/uyha/tree-sitter-cmake",		"ca627bb5828616b6246aafdc3c3222789e728e37",		"",		["cmake"]),
    ("cpp",		"https://github.com/tree-sitter/tree-sitter-cpp",		"8b5b49eb196bec7040441bee33b2c9a4838d6967",		"",		["cpp", "c++"]),
    ("css",		"https://github.com/tree-sitter/tree-sitter-css",		"dda5cfc5722c429eaba1c910ca32c2c0c5bb1a3f",		"",		["css"]),
    ("dart",		"https://github.com/UserNobody14/tree-sitter-dart",		"be07cf7118d3dba06236a3f19541685a68209934",		"",		["dart"]),
    ("dockerfile",		"https://github.com/camdencheek/tree-sitter-dockerfile",		"971acdd908568b4531b0ba28a445bf0bb720aba5",		"",		["dockerfile", "docker"]),
    ("go",		"https://github.com/tree-sitter/tree-sitter-go",		"2346a3ab1bb3857b48b29d779a1ef9799a248cd7",		"",		["go"]),
    ("haskell",		"https://github.com/tree-sitter/tree-sitter-haskell",		"0975ef72fc3c47b530309ca93937d7d143523628",		"",		["haskell"]),
    ("html",		"https://github.com/tree-sitter/tree-sitter-html",		"73a3947324f6efddf9e17c0ea58d454843590cc0",		"",		["html"]),
    ("ini",		"https://github.com/justinmk/tree-sitter-ini",		"e4018b5176132b4f3c5d6e61cea383f42288d0f5",		"",		["ini"]),
    ("java",		"https://github.com/tree-sitter/tree-sitter-java",		"e10607b45ff745f5f876bfa3e94fbcc6b44bdc11",		"",		["java"]),
    ("javascript",		"https://github.com/tree-sitter/tree-sitter-javascript",		"58404d8cf191d69f2674a8fd507bd5776f46cb11",		"",		["javascript"]),
    ("json",		"https://github.com/tree-sitter/tree-sitter-json",		"254c42a6476413b776221e03982ac8ae159eeb72",		"",		["json"]),
    ("kotlin",		"https://github.com/fwcd/tree-sitter-kotlin",		"1852ea17b7f60fb3f9d84e0b1555d56b46b39fb1",		"",		["kotlin"]),
    ("lua",		"https://github.com/tree-sitter-grammars/tree-sitter-lua",		"10fe0054734eec83049514ea2e718b2a56acd0c9",		"",		["lua"]),
    ("make",		"https://github.com/tree-sitter-grammars/tree-sitter-make",		"70613f3d812cbabbd7f38d104d60a409c4008b43",		"",		["make"]),
    ("markdown",		"https://github.com/tree-sitter-grammars/tree-sitter-markdown",		"a0a00f817d02412bd92c54d316f164d827b57b5c",		"tree-sitter-markdown/src",		["markdown", "md"]),
    ("nix",		"https://github.com/cstrahan/tree-sitter-nix",		"3d0173d903e630b6e14d17f1cf79488791379ded",		"",		["nix"]),
    ("perl",		"https://github.com/ganezdragon/tree-sitter-perl",		"5b97493ce70686e22ec8b9ea261437147e342b49",		"",		["perl"]),
    ("php",		"https://github.com/tree-sitter/tree-sitter-php",		"3fda2fb9577166c6399834917f9844f30370beea",		"php/src",		["php"]),
    ("python",		"https://github.com/tree-sitter/tree-sitter-python",		"26855eabccb19c6abf499fbc5b8dc7cc9ab8bc64",		"",		["python"]),
    ("ruby",		"https://github.com/tree-sitter/tree-sitter-ruby",		"ad907a69da0c8a4f7a943a7fe012712208da6dee",		"",		["ruby"]),
    ("rust",		"https://github.com/tree-sitter/tree-sitter-rust",		"77a3747266f4d621d0757825e6b11edcbf991ca5",		"",		["rust"]),
    ("scala",		"https://github.com/tree-sitter/tree-sitter-scala",		"db390f312a54b04b13790e1767bfac32665c17ac",		"",		["scala"]),
    ("swift",		"https://github.com/alex-pinkus/tree-sitter-swift",		"0.7.3-with-generated-files",		"",		["swift"]),
    ("toml",		"https://github.com/tree-sitter-grammars/tree-sitter-toml",		"64b56832c2cffe41758f28e05c756a3a98d16f41",		"",		["toml"]),
    ("tsx",		"https://github.com/tree-sitter/tree-sitter-typescript",		"75b3874edb2dc714fb1fd77a32013d0f8699989f",		"tsx/src",		["tsx"]),
    ("typescript",		"https://github.com/tree-sitter/tree-sitter-typescript",		"75b3874edb2dc714fb1fd77a32013d0f8699989f",		"typescript/src",		["typescript"]),
    ("yaml",		"https://github.com/tree-sitter-grammars/tree-sitter-yaml",		"a1c4812a73ec5e089de8e441fdea3a921e8d5079",		"",		["yaml", "yml"]),
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