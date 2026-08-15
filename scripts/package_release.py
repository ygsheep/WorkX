#!/usr/bin/env python3
"""package_release.py — 跨平台打包 workx 发布产物

用法:
    python scripts/package_release.py                自动检测平台与架构打包
    python scripts/package_release.py --platform linux --arch arm64  指定平台/架构（交叉打包占位）
    python scripts/package_release.py --build-dir build/Release/bin  指定构建产物目录
    python scripts/package_release.py --out dist     指定输出目录（默认 dist/）

命名:
    <project>-<platform>-<arch>-<version>.<ext>
        例: workx-windows-amd64-0.3.1.zip
            workx-linux-arm64-0.3.1.tar.gz

产物:
    - workx 可执行文件（Windows: workx.exe；Linux: workx）
    - 同目录共享库（Windows: *.dll；Linux: *.so*）
    - tools/ 子目录（内置 ripgrep，运行时由 ToolRegistry 查找）
    - 附带 README.txt（版本、平台、构建信息）

平台识别:
    Windows 默认打包 .zip；Linux/macOS 默认打包 .tar.gz。
"""

import argparse
import platform
import re
import sys
import tarfile
import zipfile
from pathlib import Path

# Windows Console 默认编码（cp1252/charmap）无法编码中文输出，
# 强制以 UTF-8 输出，避免 CI 中 zh_CN 打印触发 UnicodeEncodeError。
try:
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
except Exception:
    pass

ROOT = Path(__file__).resolve().parent.parent
VERSION_CMAKE = ROOT / "cmake" / "version.cmake"


def read_version():
    """从 cmake/version.cmake 读取事实来源版本号。"""
    text = VERSION_CMAKE.read_text(encoding="utf-8")
    m = {k: int(v) for k, v in re.findall(r"set\(WORKX_VERSION_(\w+)\s+(\d+)\)", text)}
    return f"{m['MAJOR']}.{m['MINOR']}.{m['PATCH']}"


def detect_platform():
    """返回规范平台名: windows / linux / macos。"""
    sys_plat = platform.system().lower()
    if "windows" in sys_plat:
        return "windows"
    if "linux" in sys_plat:
        return "linux"
    if "darwin" in sys_plat:
        return "macos"
    return sys_plat


def detect_arch():
    """返回规范处理器架构: amd64 / arm64 / x86 / 其他（原样小写）。"""
    m = platform.machine().lower()
    if m in ("x86_64", "amd64", "x64"):
        return "amd64"
    if m in ("aarch64", "arm64", "armv8", "arm"):
        return "arm64"
    if m in ("i386", "i686", "x86"):
        return "x86"
    return m or "unknown"


def find_build_dir():
    """定位本机构建产物目录。Windows 走 build/Release/bin，否则 build/bin。"""
    if detect_platform() == "windows":
        for cand in [ROOT / "build" / "Release" / "bin",
                     ROOT / "build" / "Debug" / "bin"]:
            if (cand / "workx.exe").exists():
                return cand
    for cand in [ROOT / "build" / "bin", ROOT / "build"]:
        if (cand / "workx").exists():
            return cand
    return None


def collect_files(build_dir, plat):
    """收集要打包的文件: 可执行 + 共享库 + tools/。返回 [(源路径, 归档内相对路径)]。"""
    exe_name = "workx.exe" if plat == "windows" else "workx"
    items = []

    exe = build_dir / exe_name
    if not exe.exists():
        sys.exit(f"[ERROR] 未找到可执行文件 {exe}，请先构建（cmake --build build ...）")
    items.append((exe, Path(exe_name)))

    # 共享库（跳过测试 exe 与 tools 子目录）
    lib_pattern = "*.dll" if plat == "windows" else "*.so*"
    for lib in build_dir.glob(lib_pattern):
        items.append((lib, lib.name))

    # tools/ 子目录（内置 ripgrep 等）
    tools = build_dir / "tools"
    if tools.exists():
        for f in sorted(tools.rglob("*")):
            if f.is_file():
                items.append((f, Path("tools") / f.relative_to(tools)))

    return items


def make_readme_lines(plat, version):
    lines = [
        f"Workx {version} ({plat})",
        "=" * 40,
        "分发包由 scripts/package_release.py 生成。",
        "",
        "运行:",
        f"  {'workx.exe' if plat == 'windows' else './workx'}",
        "",
        "依赖:",
        "  - 内置 ripgrep 位于 tools/，运行时自动查找",
        "  - Windows 需同目录的 DLL；Linux 需 libcurl 等共享库（已随包附带）",
        "",
        f"构建版本: {version}",
    ]
    return "\n".join(lines) + "\n"


def pack(build_dir, plat, arch, out_dir):
    version = read_version()
    out_dir.mkdir(parents=True, exist_ok=True)

    if plat == "windows":
        archive_name = out_dir / f"workx-{plat}-{arch}-{version}.zip"
        files = collect_files(build_dir, plat)
        with zipfile.ZipFile(archive_name, "w", zipfile.ZIP_DEFLATED) as zf:
            for src, arc_name in files:
                zf.write(src, arc_name)
            zf.writestr("README.txt", make_readme_lines(plat, version))
    else:
        archive_name = out_dir / f"workx-{plat}-{arch}-{version}.tar.gz"
        files = collect_files(build_dir, plat)
        with tarfile.open(archive_name, "w:gz") as tf:
            for src, arc_name in files:
                tf.add(src, arcname=str(arc_name))
            # README 经 BytesIO 写入，避免落盘
            import io
            data = make_readme_lines(plat, version).encode("utf-8")
            info = tarfile.TarInfo("README.txt")
            info.size = len(data)
            tf.addfile(info, io.BytesIO(data))

    return archive_name


def main():
    ap = argparse.ArgumentParser(description="打包 Workx 发布产物")
    ap.add_argument("--platform", choices=["windows", "linux", "macos"],
                    default=None, help="目标平台（默认自动检测）")
    ap.add_argument("--arch", choices=["amd64", "arm64", "x86"],
                    default=None, help="目标处理器架构（默认自动检测）")
    ap.add_argument("--build-dir", default=None, help="构建产物目录")
    ap.add_argument("--out", default=str(ROOT / "dist"), help="输出目录（默认 dist/）")
    args = ap.parse_args()

    version = read_version()
    plat = args.platform or detect_platform()
    arch = args.arch or detect_arch()

    if args.build_dir:
        build_dir = Path(args.build_dir)
        if not build_dir.exists():
            sys.exit(f"[ERROR] 构建目录不存在: {build_dir}")
    else:
        build_dir = find_build_dir()
        if build_dir is None:
            sys.exit("[ERROR] 未找到构建产物，请先构建或指定 --build-dir")

    archive = pack(build_dir, plat, arch, Path(args.out))
    print(f"版本: {version}")
    print(f"平台: {plat}")
    print(f"架构: {arch}")
    print(f"产物: {archive} ({archive.stat().st_size / 1048576:.1f} MB)")


if __name__ == "__main__":
    main()