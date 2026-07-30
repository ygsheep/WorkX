#!/usr/bin/env python3
"""
图标转换脚本：从 src/icon.png 生成各平台所需格式
  - Windows: src/icon.ico （多尺寸 16/32/48/64/128/256）
  - macOS:   src/icon.icns（Apple Icon Image 格式）
  - Linux:   直接使用 PNG，无需转换

用法:
    python scripts/convert_icon.py [--force]

依赖:
    Pillow —— 缺失时自动 pip install（用户环境隔离时可能需要 --user）

退出码:
    0  成功（或已是最新且未指定 --force）
    1  失败
"""

import argparse
import subprocess
import sys
import os
from pathlib import Path


def ensure_pillow():
    """确保 Pillow 已安装，缺失则自动安装（使用国内镜像加速）。"""
    try:
        from PIL import Image  # noqa: F401
        return True
    except ImportError:
        print("[convert_icon] Pillow not found, installing...", file=sys.stderr)
        try:
            subprocess.check_call(
                [sys.executable, "-m", "pip", "install", "Pillow",
                 "--index-url", "https://pypi.tuna.tsinghua.edu.cn/simple"]
            )
            return True
        except subprocess.CalledProcessError as e:
            print(f"[convert_icon] Failed to install Pillow: {e}",
                  file=sys.stderr)
            return False


def convert_to_ico(src_png: Path, dst_ico: Path):
    """PNG -> ICO（Windows，多尺寸）"""
    from PIL import Image
    sizes = [(16, 16), (32, 32), (48, 48), (64, 64),
             (128, 128), (256, 256)]
    img = Image.open(src_png).convert("RGBA")
    img.save(dst_ico, format="ICO", sizes=sizes)
    print(f"[convert_icon] Generated {dst_ico}")


def convert_to_icns(src_png: Path, dst_icns: Path):
    """PNG -> ICNS（macOS，Apple Icon Image 格式）

    Pillow >= 10.1 支持 ICNS 写入，要求输入尺寸 >= 512x512。
    src/icon.png 是 1280x1280，满足要求。
    """
    from PIL import Image
    img = Image.open(src_png).convert("RGBA")
    # Pillow 会自动处理多尺寸嵌入
    img.save(dst_icns, format="ICNS")
    print(f"[convert_icon] Generated {dst_icns}")


def main():
    parser = argparse.ArgumentParser(
        description="Convert src/icon.png to platform icon formats"
    )
    parser.add_argument(
        "--force", action="store_true",
        help="Force regenerate even if outputs are up-to-date"
    )
    parser.add_argument(
        "--source", default=None,
        help="Source PNG path (default: <repo>/src/icon.png)"
    )
    args = parser.parse_args()

    # 定位项目根目录（脚本位于 <root>/scripts/）
    script_dir = Path(__file__).resolve().parent
    repo_root = script_dir.parent

    src_png = Path(args.source) if args.source else (repo_root / "src" / "icon.png")
    if not src_png.exists():
        print(f"[convert_icon] Source not found: {src_png}", file=sys.stderr)
        return 1

    if not ensure_pillow():
        return 1

    dst_ico = repo_root / "src" / "icon.ico"
    dst_icns = repo_root / "src" / "icon.icns"

    # 增量构建：输出存在且比源新则跳过（除非 --force）
    if not args.force:
        need_gen = False
        if not dst_ico.exists() or dst_ico.stat().st_mtime < src_png.stat().st_mtime:
            need_gen = True
        if not dst_icns.exists() or dst_icns.stat().st_mtime < src_png.stat().st_mtime:
            need_gen = True
        if not need_gen:
            print("[convert_icon] All outputs up-to-date, skip")
            return 0

    try:
        convert_to_ico(src_png, dst_ico)
    except Exception as e:
        print(f"[convert_icon] WARNING: ICO generation failed: {e}",
              file=sys.stderr)

    try:
        convert_to_icns(src_png, dst_icns)
    except Exception as e:
        # ICNS 在非 macOS 环境可能 Pillow 不支持写入，仅警告
        print(f"[convert_icon] WARNING: ICNS generation failed: {e}",
              file=sys.stderr)

    return 0


if __name__ == "__main__":
    sys.exit(main())
