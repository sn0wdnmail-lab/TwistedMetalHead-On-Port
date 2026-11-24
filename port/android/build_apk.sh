#!/usr/bin/env bash
# Packages the Android build (libps2EntryRunner.so for arm64-v8a) into a signed APK, without
# Gradle: aapt2 (manifest + resources), javac + d8 (TmhoActivity), zipalign and apksigner.
#
#   port/android/build_apk.sh <libps2EntryRunner.so> <out.apk> <keystore.jks> <key alias>
#
# The keystore's password is read from TMHO_KEYSTORE_PASS (or asked). Keep the keystore private
# and keep it: an update installs over the old app only when it is signed with the same key.
#
# Needs: ANDROID_SDK (or ANDROID_HOME; default ~/android/sdk) with build-tools 35 and platforms/android-35, the
# NDK (for llvm-strip), a JDK 11 or newer, and zip.
set -euo pipefail

if [ $# -lt 4 ]; then
    sed -n '2,13p' "$0"
    exit 1
fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LIB="$(realpath "$1")"
OUT="$(realpath -m "$2")"
KEYSTORE="$(realpath "$3")"
ALIAS="$4"
SDK="${ANDROID_SDK:-${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/android/sdk}}}"
[ -d "$SDK" ] || { echo "Android SDK not found at $SDK (set ANDROID_SDK)" >&2; exit 1; }
BUILD_TOOLS="$(ls -d "$SDK"/build-tools/35.* 2>/dev/null | sort -V | tail -1 || true)"
PLATFORM_JAR="$SDK/platforms/android-35/android.jar"
NDK="${ANDROID_NDK:-$(ls -d "$SDK"/ndk/* 2>/dev/null | sort -V | tail -1 || true)}"
STRIP="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"

for tool in "$BUILD_TOOLS/aapt2" "$BUILD_TOOLS/d8" "$BUILD_TOOLS/zipalign" "$BUILD_TOOLS/apksigner" "$PLATFORM_JAR" "$STRIP"; do
    [ -e "$tool" ] || { echo "missing: $tool" >&2; exit 1; }
done
if [ -z "${TMHO_KEYSTORE_PASS:-}" ]; then
    read -r -s -p "Keystore password: " TMHO_KEYSTORE_PASS
    echo
fi
export TMHO_KEYSTORE_PASS

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "== resources"
"$BUILD_TOOLS/aapt2" compile --dir "$HERE/res" -o "$WORK/res.zip"
"$BUILD_TOOLS/aapt2" link -o "$WORK/base.apk" -I "$PLATFORM_JAR" \
    --manifest "$HERE/AndroidManifest.xml" "$WORK/res.zip"

echo "== java"
mkdir -p "$WORK/classes" "$WORK/dex"
find "$HERE/java" -name '*.java' > "$WORK/sources.txt"
javac --release 11 -Xlint:-options -classpath "$PLATFORM_JAR" -d "$WORK/classes" @"$WORK/sources.txt"
find "$WORK/classes" -name '*.class' > "$WORK/classes.txt"
"$BUILD_TOOLS/d8" --release --min-api 28 --lib "$PLATFORM_JAR" --output "$WORK/dex" @"$WORK/classes.txt"

echo "== native library"
mkdir -p "$WORK/apk/lib/arm64-v8a"
"$STRIP" --strip-unneeded -o "$WORK/apk/lib/arm64-v8a/libps2EntryRunner.so" "$LIB"
cp "$WORK/dex/classes.dex" "$WORK/apk/classes.dex"
cp "$WORK/base.apk" "$WORK/unaligned.apk"
# The library is stored compressed (android:extractNativeLibs="true"): a much smaller download.
(cd "$WORK/apk" && zip -q -9 -r "$WORK/unaligned.apk" classes.dex lib)

echo "== align and sign"
"$BUILD_TOOLS/zipalign" -f -P 16 4 "$WORK/unaligned.apk" "$WORK/aligned.apk"
"$BUILD_TOOLS/apksigner" sign --ks "$KEYSTORE" --ks-key-alias "$ALIAS" \
    --ks-pass env:TMHO_KEYSTORE_PASS --key-pass env:TMHO_KEYSTORE_PASS \
    --out "$OUT" "$WORK/aligned.apk"
"$BUILD_TOOLS/apksigner" verify "$OUT"
ls -l "$OUT"
