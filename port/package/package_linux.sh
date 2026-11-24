#!/bin/bash
# Packages a Linux build for players:
#   <out>/TMHO-Linux.tar.gz   TMHO/ with TMHO (stripped), TMHO.sym (symbol map for the built-in
#                             profiler), README.txt and a .desktop launcher
#
# The build should use a static FFmpeg (PS2X_FFMPEG_STATIC_ROOT) and PS2X_LINUX_PORTABLE=ON, so
# the program needs only the C library, OpenGL, X11 and a Vulkan driver from the system.
#
# usage: package_linux.sh <ps2EntryRunner> <output folder>
set -euo pipefail
BIN=$(realpath "$1")
OUT=$(realpath -m "$2")
HERE=$(cd "$(dirname "$0")" && pwd)
PKG=$(mktemp -d)
trap 'rm -rf "$PKG"' EXIT
mkdir -p "$OUT" "$PKG/TMHO"
rm -f "$OUT/TMHO-Linux.tar.gz"

# "<address hex> <name>" lines (addresses relative to where the program is loaded), read by the
# profiler in performancelogs.txt.
nm -n --defined-only -C "$BIN" | awk '$2 ~ /^[tTwW]$/ {addr=$1; $1=""; $2=""; sub(/^  /,""); print addr, $0}' > "$PKG/TMHO/TMHO.sym"
strip -o "$PKG/TMHO/TMHO" "$BIN"
chmod 755 "$PKG/TMHO/TMHO"

# The Windows read-me, told for Linux.
{
    cat <<'TXT'
Linux version
  Needs a 64-bit Linux with glibc 2.38 or newer (Ubuntu 24.04, Linux Mint 22,
  Fedora 39, Debian 13, Arch, SteamOS 3.6 or newer), a Vulkan driver (Mesa
  RADV / ANV / NVK, or NVIDIA's) and an X11 or Wayland desktop (Wayland runs
  it through XWayland). Without Vulkan it falls back to the slower software
  renderer.

  Start it from a terminal in this folder with  ./TMHO  (or double-click
  TMHO.desktop after marking it trusted / "Allow launching"). If the file
  manager opens TMHO as a text file, make it executable first:
      chmod +x TMHO
  Everything the game writes (settings, saves, logs, screenshots, the
  shader cache and the "game" folder) goes into this folder, so keep it
  somewhere you can write to (your home folder, not /opt).

TXT
    sed -e 's/Double-click TMHO\.exe/Start TMHO (see "Linux version" above)/' \
        -e 's/TMHO\.exe/TMHO/g' \
        -e 's/this PC/this computer/g' \
        "$HERE/README.txt"
} > "$PKG/TMHO/README.txt"

cat > "$PKG/TMHO/TMHO.desktop" <<'DESKTOP'
[Desktop Entry]
Type=Application
Name=Twisted Metal: Head-On (port)
Comment=Native port of Twisted Metal: Head-On - Extra Twisted Edition
Exec=sh -c 'cd "$(dirname "%k")" && exec ./TMHO'
Terminal=false
Categories=Game;
DESKTOP
chmod 755 "$PKG/TMHO/TMHO.desktop"

tar -C "$PKG" -czf "$OUT/TMHO-Linux.tar.gz" TMHO
ls -la "$OUT"
