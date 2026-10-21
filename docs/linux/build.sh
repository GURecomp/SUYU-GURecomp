#!/usr/bin/env bash
# Quick build script for suyu. Usage: ./build.sh [Release|RelWithDebInfo|Debug] [extra cmake args...]
set -euo pipefail

cd "$(dirname "$(readlink -f "$0")")"

BUILD_TYPE="${1:-Release}"
[ $# -gt 0 ] && shift
BUILD_DIR="${BUILD_DIR:-build}"
JOBS="${JOBS:-$(nproc)}"

# Keep the generator an existing build dir was configured with (CMake refuses to switch).
GENERATOR=()
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ] && command -v ninja >/dev/null; then
    GENERATOR=(-G Ninja)
fi

LAUNCHER=()
command -v ccache >/dev/null && LAUNCHER=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)

cmake -S . -B "$BUILD_DIR" "${GENERATOR[@]}" "${LAUNCHER[@]}" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DENABLE_QT=ON \
    -DYUZU_TESTS=OFF \
    -DYUZU_USE_BUNDLED_VCPKG=OFF \
    -DYUZU_USE_BUNDLED_QT=OFF \
    -DENABLE_UPDATE_CHECKER=OFF \
    -DYUZU_WERROR=OFF \
    "$@"

cmake --build "$BUILD_DIR" --parallel "$JOBS"

echo "Build finished. Binaries are in $BUILD_DIR/bin"
