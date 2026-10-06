#!/bin/sh
# Builds the prebuilt Linux release: suyu plus the link kit (suyu-cmd-static's objects and
# libraries, so players can export with only gcc/clang + CMake + Ninja installed), packaged
# into publish/ by make_package.py with its personal-data scan. See docs/a32recomp/BUILDING.md.
#
# Build on an old distribution (Ubuntu 22.04 with a newer gcc, see BUILDING.md): the package
# runs on systems with the same or a newer glibc. libstdc++ is linked statically (into suyu and,
# through the kit, into every exported game), so the player's compiler version doesn't matter.
#
# Run it from a source tree at a neutral path (e.g. /opt/suyu-src): the compiler embeds source
# paths into the binaries, and the scan rejects home directories.
# Usage: build_release_package.sh [version] [forbidden word ...]
set -e
cd "$(dirname "$0")/../.."
SRC=$(pwd)
VERSION=${1:-dev}
[ $# -gt 0 ] && shift
case "$SRC" in
/home/*|/root/*) echo "[error] This tree is under a home directory. Copy it to a neutral path such as /opt/suyu-src first."; exit 1 ;;
esac

STUB="$SRC/tools/a32recomp/kit_stub"
BUILD="$SRC/build-linux"
# The static launcher is configured against an empty stub module: only its objects and
# libraries go into the kit; the export links them with the player's recompiled game.
cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_QT=ON \
  -DYUZU_TESTS=OFF -DYUZU_USE_BUNDLED_QT=OFF -DCMAKE_PREFIX_PATH="${QT_DIR:?set QT_DIR to a Qt 6 gcc_64 folder (the Docker image does)}"   -DUSE_DISCORD_PRESENCE=OFF -DENABLE_WEB_SERVICE=OFF   -DYUZU_USE_EXTERNAL_FFMPEG=ON -DFFmpeg_HWACCEL=OFF \
  -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" \
  -DCMAKE_INSTALL_RPATH='$ORIGIN/lib' -DCMAKE_BUILD_WITH_INSTALL_RPATH=ON \
  -DSUYU_CMD_RECOMP_DIR="$STUB"
cmake --build "$BUILD" --target suyu
# The static launcher's own link fails on the stub (it has no registration table); that one
# error is expected, anything else is not.
if cmake --build "$BUILD" --target suyu-cmd-static > "$BUILD/static_objs.log" 2>&1; then
  echo "[error] the stub launcher linked; expected an unresolved suyu_recomp_static_modules"
  exit 1
fi
grep -q "undefined reference to .suyu_recomp_static_modules" "$BUILD/static_objs.log" || {
  echo "[error] building the kit objects failed; see $BUILD/static_objs.log"
  exit 1
}

OUT="$SRC/publish/suyu-mhgu-$VERSION-linux-x64"
python3 "$SRC/tools/a32recomp/make_link_kit.py" "$BUILD" "$SRC/publish/link_kit_linux"
python3 "$SRC/tools/a32recomp/make_package.py" "$BUILD/bin" "$SRC/publish/link_kit_linux" "$OUT" "$@"
tar -C "$(dirname "$OUT")" -cJf "$OUT.tar.xz" "$(basename "$OUT")"
echo
echo "Release package: $OUT.tar.xz"
