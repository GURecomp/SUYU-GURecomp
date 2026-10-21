#!/usr/bin/env bash
# Rebuild suyu-cmd-static against an existing AOT export's recompiled C sources.
# The module C code is compiled once into build-aot/ (Linux objects; Windows .obj/.lib
# in the AOT folder are ignored) and after that only changed emulator code is rebuilt
# and relinked. The AOT folder itself is only read, never written.
#
# Usage: ./relink.sh [path/to/aot/exefs]   (default: $AOT_DIR or the AOT-KEEP folder)
#        COPY_TO="/path/to/game folder" ./relink.sh   # also copy the exe over the game's binary
set -euo pipefail

cd "$(dirname "$(readlink -f "$0")")"

AOT_EXEFS="${1:-${AOT_DIR:-/run/media/linxu/Main/Gaming/GU/AOT-KEEP/exefs}}"
BUILD_DIR="${BUILD_DIR:-build-aot}"
BUILD_TYPE="${BUILD_TYPE:-Release}"

[ -f "$AOT_EXEFS/recomp_registration.c" ] || { echo "no recomp_registration.c in $AOT_EXEFS" >&2; exit 1; }

# Parallel jobs: limited by threads and by available RAM (~3 GiB per job), like the exporter.
threads=$(nproc)
avail_kb=$(awk '/MemAvailable/ {print $2}' /proc/meminfo)
by_ram=$(( avail_kb / 1024 / 3072 ))
JOBS="${JOBS:-$(( by_ram < threads ? by_ram : threads ))}"
[ "$JOBS" -ge 1 ] || JOBS=1
echo "Using $JOBS parallel jobs"

GENERATOR=()
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ] && command -v ninja >/dev/null; then GENERATOR=(-G Ninja); fi

LAUNCHER=()
command -v ccache >/dev/null && LAUNCHER=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)

cmake -S . -B "$BUILD_DIR" "${GENERATOR[@]}" "${LAUNCHER[@]}" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DENABLE_QT=OFF \
    -DYUZU_TESTS=OFF \
    -DYUZU_USE_BUNDLED_QT=OFF \
    -DENABLE_UPDATE_CHECKER=OFF \
    -DYUZU_WERROR=OFF \
    -DSUYU_CMD_RECOMP_DIR="$AOT_EXEFS"

cmake --build "$BUILD_DIR" --target suyu-cmd-static --parallel "$JOBS"

OUT="$BUILD_DIR/bin/suyu-cmd-static"
echo "Linked: $OUT"

if [ -n "${COPY_TO:-}" ]; then
    cp -v "$OUT" "$COPY_TO/$(basename "$COPY_TO")"
fi
