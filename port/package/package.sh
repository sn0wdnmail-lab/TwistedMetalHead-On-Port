#!/bin/bash
# Packages a Windows build for players:
#   <out>/TMHO.zip     TMHO.exe (stripped) + TMHO.sym (symbol map for the built-in profiler) + README.txt
#                      + recomplauncher.json (RecompLauncher's setup and settings for the game)
#   <out>/Install.bat  unpacks TMHO.zip with Windows' own tar, then deletes the zip and itself
# With a version (V9.2, say), also the file for the GitHub release, named with the system and the
# processor so RecompLauncher picks it for Windows PCs:
#   <out>/TMHO-Port-<version>-Windows-x64.zip   the same four files in a "TMHO Port <version>" folder
#
# usage: package.sh <ps2EntryRunner.exe> <output folder> [version]
set -euo pipefail
EXE=$(realpath "$1")
OUT=$(realpath -m "$2")
VERSION=${3:-}
HERE=$(cd "$(dirname "$0")" && pwd)
PKG=$(mktemp -d)
trap 'rm -rf "$PKG"' EXIT
mkdir -p "$OUT"
rm -f "$OUT/TMHO.zip" "$OUT/Install.bat"

x86_64-w64-mingw32-strip -o "$PKG/TMHO.exe" "$EXE"
# "<rva hex> <name>" lines, read by the profiler in performancelogs.txt.
base=$(x86_64-w64-mingw32-objdump -p "$EXE" | awk '/^ImageBase/ {print $2}')
x86_64-w64-mingw32-nm -n --defined-only -C "$EXE" | python3 -c "
import sys
base = int('$base', 16)
out = []
for line in sys.stdin:
    parts = line.rstrip('\n').split(' ', 2)
    if len(parts) < 3 or parts[1] not in ('t', 'T'):
        continue
    addr = int(parts[0], 16)
    if addr < base:
        continue
    out.append('%x %s' % (addr - base, parts[2]))
sys.stdout.write('\n'.join(out) + '\n')
" > "$PKG/TMHO.sym"
sed 's/$/\r/' "$HERE/README.txt" > "$PKG/README.txt"
# RecompLauncher reads it from next to the program in every release.
cp "$HERE/../../recomplauncher.json" "$PKG/recomplauncher.json"
(cd "$PKG" && zip -q -9 TMHO.zip TMHO.exe TMHO.sym README.txt recomplauncher.json)
cp "$PKG/TMHO.zip" "$OUT/TMHO.zip"
if [ -n "$VERSION" ]; then
    RELEASE="TMHO-Port-$VERSION-Windows-x64.zip"
    mkdir -p "$PKG/release/TMHO Port $VERSION"
    cp "$PKG/TMHO.exe" "$PKG/TMHO.sym" "$PKG/README.txt" "$PKG/recomplauncher.json" "$PKG/release/TMHO Port $VERSION/"
    rm -f "$OUT/$RELEASE"
    (cd "$PKG/release" && zip -q -9 -r "$OUT/$RELEASE" "TMHO Port $VERSION")
fi
cat > "$OUT/Install.bat" <<'BAT'
@echo off
rem Unpacks the TMHO PC port into this folder with Windows' own tar,
rem then deletes TMHO.zip and this file. Afterwards, start TMHO.exe.
cd /d "%~dp0"
tar -xf "TMHO.zip"
if not exist "TMHO.exe" powershell -NoProfile -Command "Expand-Archive -Force 'TMHO.zip' '.'"
if not exist "TMHO.exe" (
    echo Unpacking failed - TMHO.zip is still here.
    pause
    exit /b 1
)
del "TMHO.zip"
(goto) 2>nul & del "%~f0"
BAT
sed -i 's/$/\r/' "$OUT/Install.bat"
ls -la "$OUT"
