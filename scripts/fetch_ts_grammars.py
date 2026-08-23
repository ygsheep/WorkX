#!/usr/bin/env python3
"""fetch_ts_grammars.py — 从官方 wiki 自动收集可用的 tree-sitter grammar

用途:
    抓取 tree-sitter 官方 wiki 的 "List of parsers", 过滤出
    "仓库已 commit 预生成 parser.c 且 ABI 兼容" 的语言, 并自动解析出
    每个仓库默认分支的 HEAD commit 作为 tag, 生成可直接写进
    GRAMMARS 清单行 (供 gen_ts_grammars.py 使用)。

网络:
    通过 git 的 http.proxy 走代理 (git ls-remote 读取该配置), 无需额外配置。

用法:
    python scripts/fetch_ts_grammars.py --list        列出所有候选 (name/repo/abi)
    python scripts/fetch_ts_grammars.py --resolve     为候选解析 HEAD commit 并打印清单行
    python scripts/fetch_ts_grammars.py --update      解析后直接更新 gen_ts_grammars.py 的 GRAMMARS
    python scripts/fetch_ts_grammars.py --min-abi 14  指定最低 ABI (默认 14)

说明:
    - 只保留 github.com 仓库 (gitlab 等 GRAMMARS 结构也支持, 此处统一 github)
    - 过滤掉 abi 为 '-' (无预生成 parser.c) 的仓库
    - 自动去重 name (同名多仓库取 ABI 最高者)
    - HEAD commit 通过 `git ls-remote <repo> HEAD` 获取 (走 git 代理)
"""

import argparse
import re
import subprocess
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
WIKI_URL = "https://raw.githubusercontent.com/wiki/tree-sitter/tree-sitter/List-of-parsers.md"
GEN_SCRIPT = ROOT / "scripts" / "gen_ts_grammars.py"


def http_get(url: str, proxy: str | None) -> str:
    """带可选代理的 HTTP GET."""
    if proxy:
        handler = urllib.request.ProxyHandler({"http": proxy, "https": proxy})
        opener = urllib.request.build_opener(handler)
    else:
        opener = urllib.request.build_opener()
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
    with opener.open(req, timeout=60) as resp:
        return resp.read().decode("utf-8", "replace")


def git_proxy() -> str | None:
    """读取 git http.proxy 配置."""
    try:
        out = subprocess.run(["git", "config", "--get", "http.proxy"],
                             capture_output=True, text=True, timeout=10)
        return out.stdout.strip() or None
    except Exception:
        return None


def fetch_wiki() -> str:
    proxy = git_proxy()
    return http_get(WIKI_URL, proxy)


def parse_table(text: str) -> list[tuple[str, str, str, str]]:
    """解析 wiki 表格, 返回 [(name, repo_url, abi, scanner)]."""
    rows = []
    for line in text.splitlines():
        line = line.strip()
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip("|").split("|")]
        if len(cells) < 5:
            continue
        name, url_cell, _date, abi, *_ = cells
        if name == "name":
            continue
        # url 单元格形如: [github.com/owner/repo](https://github.com/owner/repo)
        m = re.search(r"https?://([^\s)\]]+)", url_cell)
        if not m:
            continue
        url = m.group(1).rstrip("/")
        rows.append((name, url, abi, ""))
    return rows


def filter_github(rows):
    """只保留 github.com 仓库."""
    return [(n, u, a, s) for (n, u, a, s) in rows if "github.com" in u]


def filter_abi(rows, min_abi: int):
    """保留 abi 为数字且 >= min_abi 的行 (abi 为 '-' 表示无预生成 parser.c)."""
    out = []
    for name, url, abi, scanner in rows:
        if abi.isdigit() and int(abi) >= min_abi:
            out.append((name, url, abi, scanner))
    return out


def dedupe(rows):
    """同名 grammar 取 ABI 最高者 (保留首个最高)."""
    by_name: dict[str, tuple] = {}
    for name, url, abi, scanner in rows:
        cur = by_name.get(name)
        if cur is None or int(abi) > int(cur[2]):
            by_name[name] = (name, url, abi, scanner)
    return list(by_name.values())


def head_commit(repo_url: str) -> str | None:
    """git ls-remote 取默认分支 HEAD commit."""
    url = repo_url
    # wiki 里的 url 形如 "github.com/owner/repo" (无协议), 补全 https 前缀
    if not url.startswith(("http://", "https://", "git://")):
        url = "https://" + url
    if not url.endswith(".git"):
        url += ".git"
    try:
        out = subprocess.run(["git", "ls-remote", url, "HEAD"],
                             capture_output=True, text=True, timeout=60)
        m = re.search(r"^([0-9a-f]{40})\s+HEAD", out.stdout, re.M)
        return m.group(1) if m else None
    except Exception:
        return None


def grammar_name(repo_url: str) -> str:
    """从仓库 URL 推断 grammar 名 (tree-sitter-xxx -> xxx)."""
    base = repo_url.rstrip("/").split("/")[-1]
    base = base.removesuffix(".git")
    return base.removeprefix("tree-sitter-")


def to_repo_url(repo_url: str) -> str:
    """把 URL 统一为完整 https://github.com/... 形式 (供 CMake GIT_REPOSITORY)."""
    if repo_url.startswith(("http://", "https://")):
        return repo_url
    return "https://" + repo_url


def to_grammar_line(name: str, repo_url: str, tag: str, subdir: str = "",
                    aliases: list[str] | None = None) -> str:
    if aliases is None:
        aliases = [name]
    alias_repr = ", ".join(f'"{a}"' for a in aliases)
    full_url = to_repo_url(repo_url)
    return (f'    ("{name}",\t\t"{full_url}",\t\t"{tag}",\t\t"{subdir}",\t\t'
            f'[{alias_repr}]),')


def resolve_all(rows):
    """为每个候选解析 HEAD commit, 返回填入 tag 的行."""
    resolved = []
    for name, url, abi, scanner in rows:
        tag = head_commit(url)
        resolved.append((name, url, tag, abi))
    return resolved


# ============================================================================
# 常用语言白名单 — 只自动加入主流语言, 避免 configure 拉取过多仓库
# 与 gen_ts_grammars.py 的 GRAMMARS 保持一致 (30 种主流语言)
# ============================================================================
COMMON_LANGS = {
    "c", "cpp", "bash", "json",
    # "c_sharp", "cmake", "python", "javascript",
    # "go", "typescript", "tsx", "html", "css", "markdown", "java",
    # "yaml", "toml", "dockerfile", "lua", "ruby", "php",
    # "kotlin", "swift", "scala", "perl", "dart", "haskell",
    # "ini", "make", "rust",
}

# ----------------------------------------------------------------------------
# 强制包含清单: name -> (repo_url, tag 或 None)
#   - 覆盖 dedupe 选错 / wiki 缺失的情况
#   - tag 为 None 时用 git ls-remote 解析默认分支 HEAD commit
# 例: wiki 把 tree-sitter-blade 的 name 也标成 "html" 且 ABI 更高, dedupe 会
#     挤掉真正的 tree-sitter-html; 这里强制指定 html 走官方仓库。
#     tree-sitter-swift 不在 wiki 列表 (ABI<14), 且默认分支是构建期生成,
#     故钉 release tag 0.7.3-with-generated-files。
# ----------------------------------------------------------------------------
ALWAYS_INCLUDE = {
    # "html": ("github.com/tree-sitter/tree-sitter-html", None),
    # "nix":  ("github.com/cstrahan/tree-sitter-nix", None),   # 不在 wiki 官方列表, 手工指定
    # "swift": ("github.com/alex-pinkus/tree-sitter-swift", "0.7.3-with-generated-files"),
}

# 需要改名/补 alias/subdir 的语言 (name -> dict)
LANG_OVERRIDES = {
    "c_sharp": {"name": "c-sharp", "aliases": ["c-sharp"]},
    "typescript": {"subdir": "typescript/src"},
    "tsx": {"subdir": "tsx/src"},
    "php": {"subdir": "php/src"},
    "markdown": {"subdir": "tree-sitter-markdown/src", "aliases": ["markdown", "md"]},
    "yaml": {"aliases": ["yaml", "yml"]},
    "dockerfile": {"aliases": ["dockerfile", "docker"]},
    "cpp": {"aliases": ["cpp", "c++"]},
    "toml": {},
}


def apply_overrides(name: str, url: str, tag: str) -> tuple[str, str, str, list]:
    """应用改名/别名/subdir 覆盖, 返回 (name, url, subdir, aliases)."""
    ov = LANG_OVERRIDES.get(name, {})
    new_name = ov.get("name", name)
    subdir = ov.get("subdir", "")
    aliases = ov.get("aliases", [new_name])
    return new_name, url, subdir, aliases


def main() -> int:
    ap = argparse.ArgumentParser(description="收集可用 tree-sitter grammar")
    ap.add_argument("--list", action="store_true", help="列出全部候选")
    ap.add_argument("--list-common", action="store_true", help="列出白名单内候选")
    ap.add_argument("--resolve", action="store_true", help="解析 HEAD commit 并打印清单行")
    ap.add_argument("--update", action="store_true", help="更新 gen_ts_grammars.py 的 GRAMMARS")
    ap.add_argument("--min-abi", type=int, default=14, help="最低 ABI (默认 14)")
    ap.add_argument("--all", action="store_true", help="忽略白名单, 处理全部候选")
    args = ap.parse_args()

    print("抓取官方 wiki ...")
    raw = fetch_wiki()
    rows = parse_table(raw)
    rows = filter_github(rows)
    rows = filter_abi(rows, args.min_abi)
    rows = dedupe(rows)
    rows.sort(key=lambda r: r[0])
    print(f"候选 {len(rows)} 个 (github + ABI>={args.min_abi} + 预生成 parser.c)\n")

    if args.list:
        for name, url, abi, _ in rows:
            print(f"  {name:<24} ABI={abi:<3} {url}")
        return 0

    # 白名单过滤 (默认); --all 时处理全部
    if args.all:
        selected = rows
        forced = []  # (name, url, tag)
    else:
        # 白名单内 + 强制包含 (覆盖 dedupe 选错 / wiki 缺失)
        by_name = {r[0]: r for r in rows}
        selected = [r for r in rows if r[0] in COMMON_LANGS]
        forced = []
        for name, (url, tag) in ALWAYS_INCLUDE.items():
            if tag:
                # 已钉 tag 的强制项 (如 swift), 直接解析, 不进 selected
                forced.append((name, url, tag, ""))
                continue
            if name in by_name and by_name[name][1] != url:
                # 用强制 url 覆盖候选里可能选错的仓库
                selected = [r for r in selected if r[0] != name]
                selected.append((name, url, "ABI?", ""))
            elif name not in by_name:
                selected.append((name, url, "ABI?", ""))
        selected.sort(key=lambda r: r[0])
    print(f"处理 {len(selected)} 个 (白名单, 含 {len(forced)} 个钉版本) ...\n")

    if args.list_common:
        for name, url, _abi, _ in selected:
            print(f"  {name:<24} {url}")
        return 0

    resolved = resolve_all(selected)
    failed = [r for r in resolved if not r[2]]
    if failed:
        print(f"警告: {len(failed)} 个仓库无法解析 HEAD commit:")
        for name, url, _, _ in failed:
            print(f"  - {name}  {url}")
        print()

    # 合并已钉 tag 的强制项
    resolved += forced

    # 应用改名/别名/subdir, 生成最终清单行
    lines = []
    for name, url, tag, _abi in resolved:
        if not tag:
            continue
        new_name, url, subdir, aliases = apply_overrides(name, url, tag)
        lines.append(to_grammar_line(new_name, url, tag, subdir, aliases))
    lines.sort()

    print(f"成功生成 {len(lines)} 个清单行:\n")
    for ln in lines:
        print(ln)

    if args.update:
        print(f"\n更新 {GEN_SCRIPT.relative_to(ROOT)} ...")
        _rewrite_grammars(lines)
        print("完成.")
    return 0


def _rewrite_grammars(new_lines: list[str]):
    """把 GRAMMARS 列表体重写为新行 (保留标题注释与收尾)."""
    src = GEN_SCRIPT.read_text(encoding="utf-8")
    start = src.index("GRAMMARS = [") + len("GRAMMARS = [")
    # 列表闭合的 ] 是列表体之后第一个独占一行 (前导空格 + ]) 的 ],
    # 即跳过所有元素行内的 [...] 后, 找到 "\n]"
    end = src.index("\n]", start)
    body = "\n".join(new_lines)
    new = src[:start] + "\n" + body + "\n" + src[end + 1:]
    GEN_SCRIPT.write_text(new, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    sys.exit(main())