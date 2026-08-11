#!/usr/bin/env python3
"""bump_version.py — Workx 版本管理脚本（SemVer，单一事实源 = cmake/version.cmake）

用法:
    python scripts/bump_version.py show             # 打印当前版本
    python scripts/bump_version.py auto             # 按文件级版本聚合自动升 minor/patch（不满足则不动）
    python scripts/bump_version.py patch            # 0.2.0 -> 0.2.1（兼容修复）
    python scripts/bump_version.py minor            # 0.2.0 -> 0.3.0（不兼容变更）
    python scripts/bump_version.py major            # 0.2.0 -> 1.0.0（正式发布）

版本提升规则（0.x 阶段，与 cmake/version.cmake 头部注释一致）:
    MAJOR = 0 固定，正式 1.0 前不升
    MINOR = 不兼容变更（公共 API 破坏、行为不兼容）
    PATCH = 兼容修复（bugfix、无 API 变化的内部优化）
    auto = 依赖 scripts/version_files.py 的文件级 @version 聚合:
           任一文件大改(+0.1) → minor；小改/新增数 >= 10 → patch

同步文件:
    cmake/version.cmake           （事实来源）
    vcpkg.json                    （vcpkg manifest）
    tests/consumer/vcpkg.json     （消费方测试）
    flake.nix                     （Nix flake 打包）
    nix/workx.nix                 （Nix 打包）
"""

import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VERSION_CMAKE = ROOT / "cmake" / "version.cmake"

# 同步目标：(相对路径, 旧版本模式, 新版本替换) —— 替换后 CMake configure 会再次校验
SYNC_FILES = [
    ("vcpkg.json",
     r'"version-string": "{cur}"', r'"version-string": "{new}"'),
    ("tests/consumer/vcpkg.json",
     r'"version-string": "{cur}"', r'"version-string": "{new}"'),
    ("flake.nix",
     r'version = "{cur}";', r'version = "{new}";'),
    ("nix/workx.nix",
     r'version = "{cur}";', r'version = "{new}";'),
]


def read_version():
    text = VERSION_CMAKE.read_text(encoding="utf-8")
    m = {k: int(v) for k, v in re.findall(r"set\(WORKX_VERSION_(\w+)\s+(\d+)\)", text)}
    return (m["MAJOR"], m["MINOR"], m["PATCH"])


def last_vtag():
    r = subprocess.run(["git", "describe", "--tags", "--abbrev=0", "--match", "v[0-9]*"],
                       capture_output=True, text=True, encoding="utf-8", errors="replace", cwd=ROOT)
    return r.stdout.strip() if r.returncode == 0 and r.stdout.strip() else None


def auto_decision():
    """依据文件级版本聚合结果（version_files.py --stat）自动决定 minor/patch/none。"""
    tag = last_vtag()
    argv = [sys.executable, str(ROOT / "scripts" / "version_files.py"), "--stat"]
    if tag:
        argv += ["--since", tag]
    r = subprocess.run(argv, capture_output=True, text=True, encoding="utf-8", errors="replace", cwd=ROOT)
    if r.returncode != 0:
        print(r.stderr, file=sys.stderr)
        return "none"
    try:
        return json.loads(r.stdout.strip())["bump"]
    except (json.JSONDecodeError, KeyError):
        return "none"


def main():
    if len(sys.argv) != 2:
        print(__doc__.strip())
        sys.exit(1)
    cmd = sys.argv[1].lower()

    major, minor, patch = read_version()
    cur = f"{major}.{minor}.{patch}"

    if cmd == "show":
        print(cur)
        return

    if cmd == "auto":
        decision = auto_decision()
        if decision == "none":
            print("文件版本变化不足，无需升版（任一文件大改→minor；小改/新增≥10→patch）")
            return
        cmd = decision
        print(f"文件版本聚合: {decision}")

    if cmd == "major":
        major, minor, patch = major + 1, 0, 0
    elif cmd == "minor":
        minor, patch = minor + 1, 0
    elif cmd == "patch":
        patch += 1
    else:
        print(f"未知命令: {cmd}（支持 show | auto | major | minor | patch）", file=sys.stderr)
        sys.exit(1)
    new = f"{major}.{minor}.{patch}"

    # 1. 事实来源
    text = VERSION_CMAKE.read_text(encoding="utf-8")
    text = re.sub(r"set\(WORKX_VERSION_MAJOR\s+\d+\)", f"set(WORKX_VERSION_MAJOR {major})", text)
    text = re.sub(r"set\(WORKX_VERSION_MINOR\s+\d+\)", f"set(WORKX_VERSION_MINOR {minor})", text)
    text = re.sub(r"set\(WORKX_VERSION_PATCH\s+\d+\)", f"set(WORKX_VERSION_PATCH {patch})", text)
    VERSION_CMAKE.write_text(text, encoding="utf-8")

    # 2. 同步下游文件
    synced = []
    for rel, old_pat, new_pat in SYNC_FILES:
        p = ROOT / rel
        if not p.exists():
            print(f"  跳过（不存在）: {rel}")
            continue
        old_pat = old_pat.format(cur=cur, new=new)
        new_pat = new_pat.format(cur=cur, new=new)
        t = p.read_text(encoding="utf-8")
        if not re.search(old_pat, t):
            print(f"  警告: {rel} 中未找到 {old_pat!r}，跳过（configure 时会报错）")
            continue
        p.write_text(re.sub(old_pat, new_pat, t), encoding="utf-8")
        synced.append(rel)

    print(f"{cur} -> {new}")
    print(f"  已同步: {', '.join(synced)}")
    print(f"  提示: 发布后打 tag → git tag v{new} && git push origin v{new}（构建版本号将带上 tag）")


if __name__ == "__main__":
    main()
