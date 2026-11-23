#!/usr/bin/env bash
# Builds the Twisted Metal: Head-On PC port from your own copy of the game.
#
# usage: port/build.sh <SCUS_976.21> [windows|linux] [build dir]
#
#   <SCUS_976.21>  the game's executable, copied from your own disc (SCUS-97621, NTSC-U).
#   windows        (default) cross-builds TMHO.exe with mingw-w64 on a Linux host.
#   linux          builds a native Linux binary with clang.
#
# Needs: git, cmake >= 3.21, ninja, python3, clang (Linux build) or mingw-w64 g++-posix (Windows
# build), the Vulkan headers, and FFmpeg for the movies: the libavcodec/libavformat/libswscale/
# libswresample dev packages on Linux, or a static FFmpeg built for x86_64-w64-mingw32 for Windows
# (FFMPEG_ROOT; see BUILDING.md for the configure line).
#
# Nothing of the game is in this repository: the recompiler turns your executable into C++ in
# ps2recomp/ps2xRuntime/src/runner, and that output stays on your machine.
set -euo pipefail

ELF=$(realpath "$1")
TARGET=${2:-windows}
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
WORK=$(realpath -m "${3:-$ROOT/build}")
SRC="$ROOT/ps2recomp"
PGS="$SRC/ps2xRuntime/third_party/parallel-gs"

# 1. paraLLEl-GS (the Vulkan GS renderer) at the tested commit, with the port's small patch.
git -C "$ROOT" submodule update --init --recursive
if ! git -C "$PGS" apply --reverse --check "$HERE/patches/parallel-gs.patch" 2>/dev/null; then
    git -C "$PGS" apply "$HERE/patches/parallel-gs.patch"
fi

# 2. The recompiler (host tool).
cmake -S "$SRC" -B "$WORK/tools" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_ANALYZER=OFF
cmake --build "$WORK/tools" --target ps2_recomp

# 3. Recompile the game's executable to C++ (function map and patches in port/tmho.toml).
mkdir -p "$HERE/game"
cp -f "$ELF" "$HERE/game/SCUS_976.21"
(cd "$HERE" && rm -rf output && "$WORK/tools/ps2xRecomp/ps2_recomp" tmho.toml > "$WORK/recomp.log")
find "$SRC/ps2xRuntime/src/runner" -name '*.cpp' ! -name 'register_functions.cpp' -delete
cp "$HERE"/output/*.cpp "$SRC/ps2xRuntime/src/runner/"
cp "$HERE"/output/*.h "$SRC/ps2xRuntime/include/"

# 4. The game program.
COMMON=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DPS2X_BUILD_RECOMP=OFF -DPS2X_BUILD_ANALYZER=OFF
        -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF -DPS2X_ENABLE_LTO=OFF
        -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF -DPS2X_ENABLE_SCCACHE=OFF)
if [ "$TARGET" = linux ]; then
    cmake -S "$SRC" -B "$WORK/linux" "${COMMON[@]}" -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
        -DCMAKE_CXX_FLAGS="-msse4.2" -DCMAKE_C_FLAGS="-msse4.2"
    cmake --build "$WORK/linux" --target ps2EntryRunner
    echo "Built $WORK/linux/ps2xRuntime/ps2EntryRunner"
else
    : "${FFMPEG_ROOT:?set FFMPEG_ROOT to a static FFmpeg built for x86_64-w64-mingw32}"
    cmake -S "$SRC" -B "$WORK/win" "${COMMON[@]}" \
        -DCMAKE_TOOLCHAIN_FILE="$HERE/cmake/mingw-w64-x86_64.cmake" \
        -DCMAKE_CXX_FLAGS_RELEASE="-O2 -DNDEBUG" -DCMAKE_C_FLAGS_RELEASE="-O2 -DNDEBUG" \
        -DCMAKE_CXX_FLAGS="-msse4.2 -Wa,-mbig-obj" -DCMAKE_C_FLAGS="-msse4.2" \
        -DCMAKE_EXE_LINKER_FLAGS="-static -static-libgcc -static-libstdc++" \
        -DPS2X_FFMPEG_STATIC_ROOT="$FFMPEG_ROOT" -DPS2X_SHOW_WINDOWS_CONSOLE=OFF
    cmake --build "$WORK/win" --target ps2EntryRunner
    "$HERE/package/package.sh" "$WORK/win/ps2xRuntime/ps2EntryRunner.exe" "$WORK/package"
fi
