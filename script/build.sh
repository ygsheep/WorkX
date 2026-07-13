#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR="${1:-_build}"

cmake -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_TOOLCHAIN_FILE="$HOME/WorkSpace/vcpkg/scripts/buildsystems/vcpkg.cmake"

cmake --build "$BUILD_DIR" -j "$(nproc)"
