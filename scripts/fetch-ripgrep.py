#!/usr/bin/env python3
"""fetch-ripgrep.py — 跨平台下载 ripgrep 二进制到 vendor/ripgrep/<platform>/

用法:
    python scripts/fetch-ripgrep.py [--version 14.1.1]

作用:
    检测当前平台,从 GitHub Release 下载对应 ripgrep,解压 rg 到
    vendor/ripgrep/{windows,macos,linux}/rg(.exe)。

    CMakeLists 在 POST_BUILD 阶段会把该二进制拷贝到
    $<TARGET_FILE_DIR:workx>/tools/,运行时由 ToolRegistry 自动发现。

幂等:
    目标文件已存在则跳过,重复执行无副作用。

CI 用法:
    GitHub Actions / GitLab CI 在 cmake configure 前执行本脚本,
    确保 vendor/ripgrep/<platform>/ 就绪,触发 CMakeLists 的
    add_custom_command POST_BUILD 拷贝。
"""

import argparse
import platform
import shutil
import stat
import sys
import tarfile
import urllib.request
import zipfile
from pathlib import Path

RIPGREP_VERSION = "14.1.1"
# 脚本位于 scripts/,项目根在上一级
VENDOR_DIR = Path(__file__).resolve().parent.parent / "vendor" / "ripgrep"


def detect_target():
    """返回 (platform_name, target_triple, archive_ext, bin_name)。

    platform_name 对齐 CMakeLists 期望的 vendor/ripgrep/<platform>/ 目录命名。
    target_triple 对齐 ripgrep GitHub Release 资产命名。
    """
    system = platform.system()
    machine = platform.machine().lower()

    if system == "Windows":
        # ripgrep Windows 发布仅提供 x86_64
        return "windows", "x86_64-pc-windows-msvc", ".zip", "rg.exe"
    if system == "Darwin":
        arch = "aarch64" if machine in ("arm64", "aarch64") else "x86_64"
        return "macos", f"{arch}-apple-darwin", ".tar.gz", "rg"
    if system == "Linux":
        arch = "aarch64" if machine in ("aarch64", "arm64") else "x86_64"
        # musl 静态链接版本,无 glibc 依赖,兼容性更好
        return "linux", f"{arch}-unknown-linux-musl", ".tar.gz", "rg"

    raise RuntimeError(f"不支持的平台: {system}")


def download(url, dest):
    """下载文件到 dest。"""
    print(f"下载: {url}")
    urllib.request.urlretrieve(url, dest)
    print(f"已保存: {dest}")


def extract_and_find(archive_path, extract_dir, bin_name):
    """解压归档并在解压目录中查找 bin_name,返回其 Path。

    ripgrep Release 归档结构:
        ripgrep-<version>-<triple>/rg(.exe)          # 主二进制
        ripgrep-<version>-<triple>/doc/...           # 文档
        ripgrep-<version>-<triple>/complete/...      # 补全脚本
    """
    if archive_path.suffix == ".zip":
        with zipfile.ZipFile(archive_path) as zf:
            zf.extractall(extract_dir)
    else:
        with tarfile.open(archive_path, "r:gz") as tf:
            tf.extractall(extract_dir)

    for candidate in extract_dir.rglob(bin_name):
        if candidate.is_file():
            return candidate

    raise RuntimeError(f"解压后未找到 {bin_name}")


def main():
    parser = argparse.ArgumentParser(description="下载 ripgrep 二进制到 vendor/ripgrep/")
    parser.add_argument(
        "--version",
        default=RIPGREP_VERSION,
        help=f"ripgrep 版本 (默认 {RIPGREP_VERSION})",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="强制重新下载,即使目标已存在",
    )
    args = parser.parse_args()

    plat_name, triple, ext, bin_name = detect_target()
    dest_dir = VENDOR_DIR / plat_name
    dest_bin = dest_dir / bin_name

    # 幂等:已存在则跳过
    if dest_bin.exists() and not args.force:
        print(f"已存在,跳过: {dest_bin}")
        print("如需重新下载,使用 --force")
        return 0

    dest_dir.mkdir(parents=True, exist_ok=True)

    archive_name = f"ripgrep-{args.version}-{triple}{ext}"
    url = (
        f"https://github.com/BurntSushi/ripgrep/releases/download/"
        f"{args.version}/{archive_name}"
    )
    archive_path = dest_dir / archive_name

    # 下载
    download(url, archive_path)

    # 解压并定位二进制
    extract_dir = dest_dir / "_extract"
    if extract_dir.exists():
        shutil.rmtree(extract_dir, ignore_errors=True)
    extract_dir.mkdir(exist_ok=True)

    try:
        rg_path = extract_and_find(archive_path, extract_dir, bin_name)
        # 移动到最终位置
        if dest_bin.exists():
            dest_bin.unlink()
        shutil.move(str(rg_path), str(dest_bin))

        # POSIX 设置可执行权限
        if plat_name != "windows":
            mode = dest_bin.stat().st_mode
            dest_bin.chmod(mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)
    finally:
        # 清理临时解压目录和归档
        shutil.rmtree(extract_dir, ignore_errors=True)
        archive_path.unlink(missing_ok=True)

    print(f"完成: {dest_bin}")
    print(f"版本: ripgrep {args.version} ({triple})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
