#!/usr/bin/env python3
"""fetch-jq.py — 跨平台下载 jq 二进制到 vendor/jq/<platform>/

用法:
    python scripts/fetch-jq.py [--version 1.7.1]

作用:
    检测当前平台，从 GitHub Release 下载对应的 jq 单文件二进制到
    vendor/jq/{windows,macos,linux}/jq(.exe)。

    CMakeLists 在 POST_BUILD 阶段会将该二进制拷贝到
    $<TARGET_FILE_DIR:workx>/tools/，运行时由 ToolRegistry 的
    resolve_jq() 自动发现。

幂等:
    目标文件已存在则跳过，重复执行无副作用。

CI 用法:
    GitHub Actions / GitLab CI 在 cmake configure 前执行本脚本，
    确保 vendor/jq/<platform>/ 就绪，触发 CMakeLists 的
    add_custom_command POST_BUILD 拷贝。

说明:
    jq 官方 Release 提供单文件二进制（无需解压）：
        jq-windows-amd64.exe / jq-linux-amd64 / jq-macos-amd64
    仅提供 x86_64 官方构建；非 amd64 架构会给出提示并退出。
"""

import argparse
import platform
import stat
import sys
import urllib.request
from pathlib import Path

# Windows Console 默认编码无法编码中文输出，强制 UTF-8。
try:
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
except Exception:
    pass

JQ_VERSION = "1.7.1"
# 脚本位于 scripts/，项目根在上一级
VENDOR_DIR = Path(__file__).resolve().parent.parent / "vendor" / "jq"


def detect_target():
    """返回 (platform_name, asset_suffix, bin_name)。

    platform_name 对齐 CMakeLists 期望的 vendor/jq/<platform>/ 目录命名。
    asset_suffix 对应 jq GitHub Release 单文件资产后缀。
    """
    system = platform.system()
    arch = platform.machine().lower()

    if arch not in ("amd64", "x86_64"):
        raise RuntimeError(f"jq 官方仅提供 x86_64 构建，当前架构 {platform.machine()} 不受支持")

    if system == "Windows":
        return "windows", "windows-amd64.exe", "jq.exe"
    if system == "Darwin":
        return "macos", "macos-amd64", "jq"
    if system == "Linux":
        return "linux", "linux-amd64", "jq"

    raise RuntimeError(f"不支持的平台: {system}")


def download(url, dest):
    """下载文件到 dest。"""
    print(f"下载: {url}")
    urllib.request.urlretrieve(url, dest)
    print(f"已保存: {dest}")


def main():
    parser = argparse.ArgumentParser(description="下载 jq 二进制到 vendor/jq/")
    parser.add_argument(
        "--version",
        default=JQ_VERSION,
        help=f"jq 版本 (默认 {JQ_VERSION})",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="强制重新下载，即使目标已存在",
    )
    args = parser.parse_args()

    plat_name, asset_suffix, bin_name = detect_target()
    dest_dir = VENDOR_DIR / plat_name
    dest_bin = dest_dir / bin_name

    # 幂等：已存在则跳过
    if dest_bin.exists() and not args.force:
        print(f"已存在，跳过: {dest_bin}")
        print("如需重新下载，使用 --force")
        return 0

    dest_dir.mkdir(parents=True, exist_ok=True)
    # 下载到临时文件，避免中断留下半成品
    tmp = dest_dir / f".{bin_name}.download"

    url = (
        f"https://github.com/jqlang/jq/releases/download/"
        f"jq-{args.version}/jq-{asset_suffix}"
    )

    try:
        download(url, tmp)
        if dest_bin.exists():
            dest_bin.unlink()
        tmp.rename(dest_bin)
    finally:
        tmp.unlink(missing_ok=True)

    # POSIX 设置可执行权限
    if plat_name != "windows":
        mode = dest_bin.stat().st_mode
        dest_bin.chmod(mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)

    print(f"完成: {dest_bin}")
    print(f"版本: jq {args.version}")
    return 0


if __name__ == "__main__":
    sys.exit(main())