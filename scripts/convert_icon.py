#!/usr/bin/env python3
"""
图标转换脚本：从 src/icon.png 生成各平台所需格式
  - Windows: src/icon.ico （多尺寸 16/32/48/64/128/256）
  - macOS:   src/icon.icns（Apple Icon Image 格式）
  - Linux:   直接使用 PNG，无需转换

用法:
    python scripts/convert_icon.py [--force]

依赖:
    Pillow —— 缺失时不自动安装，仅提示开发者手动安装：
                  pip install Pillow

退出码:
    0  至少一个产物生成成功（或已是最新且未指定 --force）
    1  Pillow 缺失 / 源文件缺失 / 全部产物生成失败
"""

import argparse
import sys
from pathlib import Path


def ensure_pillow():
    """检测 Pillow 是否可用。缺失则提示手动安装，不自动安装。"""
    try:
        from PIL import Image  # noqa: F401
        return True
    except ImportError:
        print(
            "[convert_icon] Pillow not found. "
            "Install manually: pip install Pillow",
            file=sys.stderr,
        )
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
    建议源图 >= 1024x1024 以覆盖 Retina 显示需求。
    """
    from PIL import Image
    img = Image.open(src_png).convert("RGBA")
    if max(img.size) < 512:
        raise RuntimeError(
            f"ICNS requires source >= 512x512, got {img.size}"
        )
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

    ico_ok = False
    icns_ok = False

    try:
        convert_to_ico(src_png, dst_ico)
        ico_ok = True
    except Exception as e:
        print(f"[convert_icon] WARNING: ICO generation failed: {e}",
              file=sys.stderr)

    try:
        convert_to_icns(src_png, dst_icns)
        icns_ok = True
    except Exception as e:
        # ICNS 在非 macOS 环境可能 Pillow 不支持写入，仅警告
        print(f"[convert_icon] WARNING: ICNS generation failed: {e}",
              file=sys.stderr)

    # 至少一个产物生成成功才算通过；全失败返回 1
    return 0 if (ico_ok or icns_ok) else 1


if __name__ == "__main__":
    sys.exit(main())
