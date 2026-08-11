#!/usr/bin/env python3
"""version_files.py — 文件级版本号管理（src/core、src/agent 文件头 @version）

两层版本体系:
    文件级 @version M.N.P（本脚本维护，随改随升）
        小改(默认)     → PATCH+1        （即 +0.0.1）
        大改(--minor)  → MINOR+1、PATCH 归零（即 +0.1）
    项目级（bump_version.py，由本脚本聚合结果驱动）
        任一文件大改            → 项目升 minor
        小改/新文件数 >= 10     → 项目升 patch
        两者都不满足           → 不升

用法:
    python scripts/version_files.py                             # 更新改动文件的 @version（小改+0.0.1）
        --since <commit|tag>                                    # 统计该提交以来的改动（默认: 工作区 vs HEAD）
        --minor <file> [--minor <file> ...]                     # 标记大改文件（MINOR+1）
        --dry-run                                               # 只预览不写入
    python scripts/version_files.py --stat [--since <ref>]      # 输出聚合统计 JSON，不改文件
        --out <json_path>                                       # 统计写入文件
"""

import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCOPES = ("src/core/", "src/agent/")
EXTENSIONS = (".h", ".hpp", ".cpp")
THRESHOLD = 10  # 小改/新文件数达到该值 → 项目升 patch

VERSION_RE = re.compile(r"@version\s+(\d+)\.(\d+)\.(\d+)")
FILE_RE = re.compile(r"^(\s*\*\s*@file[^\n]*)$", re.MULTILINE)


def git(args):
    r = subprocess.run(["git", *args], capture_output=True, text=True,
                       encoding="utf-8", errors="replace", cwd=ROOT)
    if r.returncode != 0:
        return None
    return r.stdout


def read_raw(path):
    with open(path, "r", encoding="utf-8", newline="") as f:
        return f.read()


def write_raw(path, text):
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def find_version(text):
    m = VERSION_RE.search(text)
    if m:
        return tuple(int(x) for x in m.groups())
    return None


def bump_version_text(text, minor=False):
    def repl(m):
        ma, mi, pa = (int(x) for x in m.groups())
        if minor:
            mi, pa = mi + 1, 0
        else:
            pa += 1
        return f"@version {ma}.{mi}.{pa}"

    new_text, n = VERSION_RE.subn(repl, text, count=1)
    return new_text, n


def insert_version(text):
    m = FILE_RE.search(text)
    if not m:
        return text, False
    line = m.group(1)
    indent = line[: line.index("*")]
    new_line = indent + "* @version 1.0.0"
    return text.replace(line, line + "\n" + new_line, 1), True


def changed_files(since):
    if since:
        out = git(["diff", "--name-only", f"{since}..HEAD"])
        files = (out or "").splitlines()
    else:
        out = git(["diff", "--name-only", "HEAD"])
        files = (out or "").splitlines()
        untracked = git(["ls-files", "--others", "--exclude-standard"])
        files += (untracked or "").splitlines()
    return [
        f for f in files
        if f.startswith(SCOPES) and f.endswith(EXTENSIONS)
    ]


def old_version(path, ref):
    if ref is None:
        return None
    out = git(["show", f"{ref}:{path}"])
    if out is None:
        return None
    return find_version(out)


def stat_files(files, since):
    """返回 {path: (old_ver, cur_ver)} 及聚合统计，不修改文件。"""
    counts = {"minor": 0, "major": 0}
    detail = {}
    for rel in files:
        p = ROOT / rel
        if not p.exists():
            continue
        cur = find_version(read_raw(p))
        old = old_version(rel, since)
        detail[rel] = (old, cur)
        if old is None and cur is None:
            continue  # 文件头一直没有 @version，未纳入统计
        if cur is None:
            cur = (1, 0, 0)  # 本次将补 1.0.0，视为小改
        if old is None:
            counts["minor"] += 1  # 新文件/新补版本号
        elif cur[0] > old[0]:
            counts["major"] += 1
        elif cur[1] > old[1]:
            counts["major"] += 1
        elif cur != old:
            counts["minor"] += 1
    return detail, counts


def decide_bump(counts):
    if counts["major"] > 0:
        return "minor"
    if counts["minor"] >= THRESHOLD:
        return "patch"
    return "none"


def main():
    ap = argparse.ArgumentParser(description="文件级版本号管理（src/core、src/agent @version）")
    ap.add_argument("--stat", action="store_true", help="只输出聚合统计 JSON，不修改文件")
    ap.add_argument("--since", metavar="REF", help="改动统计范围（默认: 工作区 vs HEAD）")
    ap.add_argument("--minor", action="append", default=[], metavar="FILE",
                    help="标记大改文件（MINOR+1），可重复")
    ap.add_argument("--dry-run", action="store_true", help="只预览不写入")
    ap.add_argument("--out", metavar="PATH", help="统计 JSON 写入文件")
    args = ap.parse_args()

    files = changed_files(args.since)
    if not files:
        print("没有改动的 core/agent 文件", file=sys.stderr)
        sys.exit(0)

    if args.stat:
        _, counts = stat_files(files, args.since)
        result = {"minor": counts["minor"], "major": counts["major"],
                  "total": counts["minor"] + counts["major"],
                  "bump": decide_bump(counts)}
        text = json.dumps(result, ensure_ascii=False)
        if args.out:
            Path(args.out).write_text(text, encoding="utf-8")
        print(text)
        return

    # 更新模式
    minor_set = {Path(f).as_posix() for f in args.minor}
    for rel in files:
        p = ROOT / rel
        if not p.exists():
            continue
        is_minor = rel in minor_set
        text = read_raw(p)
        new_text, n = bump_version_text(text, minor=is_minor)
        if n == 0:
            new_text, inserted = insert_version(text)
        else:
            inserted = False
        if new_text != text:
            kind = "大改" if is_minor else ("新增" if inserted else "小改")
            if args.dry_run:
                print(f"  [dry-run] {rel}: {kind} → 更新 @version")
            else:
                write_raw(p, new_text)
                print(f"  {rel}: {kind} → 更新 @version")

    # 预览聚合建议（相对 --since；默认相对 HEAD 的工作区改动）
    _, counts = stat_files(files, args.since)
    print(f"\n本次: 小改/新增 {counts['minor']} 个, 大改 {counts['major']} 个")
    print(f"聚合建议: {decide_bump(counts)} (小改≥{THRESHOLD}→patch; 任一文件大改→minor)")


if __name__ == "__main__":
    main()
