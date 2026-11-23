#!/bin/bash
# Packages a Windows build for players:
#   <out>/TMHO.zip     TMHO.exe (stripped) + TMHO.sym (symbol map for the built-in profiler) + README.txt
#   <out>/Install.bat  unpacks TMHO.zip with Windows' own tar, then deletes the zip and itself
#
# usage: package.sh <ps2EntryRunner.exe> <output folder>
set -euo pipefail
EXE=$(realpath "$1")
OUT=$(realpath -m "$2")
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
(cd "$PKG" && zip -q -9 TMHO.zip TMHO.exe TMHO.sym README.txt)
cp "$PKG/TMHO.zip" "$OUT/TMHO.zip"
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
