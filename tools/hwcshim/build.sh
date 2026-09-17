#!/bin/bash
# Build the HWC2 interposer for arm64 Android with the NDK.
#
#   tools/hwcshim/build.sh            -> tools/hwcshim/out/hwcomposer.mtk_common.so
#
# NDK: `brew install --cask android-ndk` puts it at /opt/homebrew/share/android-ndk.
# Override with ANDROID_NDK_HOME. The vendor is Android 12 (API 31) but the
# shim only touches liblog/libdl/libc, so API 30 is a safe floor.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NDK="${ANDROID_NDK_HOME:-/opt/homebrew/share/android-ndk}"
TC="$NDK/toolchains/llvm/prebuilt"
HOST="$(ls "$TC" | head -1)"
CXX="$TC/$HOST/bin/clang++"
[ -x "$CXX" ] || { echo "NDK clang++ not found at $CXX (set ANDROID_NDK_HOME)" >&2; exit 1; }

OUT="$HERE/out"; mkdir -p "$OUT"
SO="$OUT/hwcomposer.mtk_common.so"

"$CXX" --target=aarch64-linux-android30 \
  -std=c++17 -O2 -g -fPIC -shared \
  -fno-exceptions -fno-rtti -nostdlib++ \
  -fvisibility=hidden -Wall -Wextra -Wno-unused-parameter \
  -Wl,-z,now -Wl,-z,relro -Wl,--no-undefined -Wl,--hash-style=gnu \
  -Wl,-soname,hwcomposer.mtk_common.so \
  -o "$SO" "$HERE/hwcshim.cpp" -llog -ldl

# Vendor files are 0644 root:root; the image-build step re-asserts this, but
# keep the local artefact consistent.
chmod 0644 "$SO"
# Stripped copy for the device (38 KB vs 128 KB); the unstripped one is for
# symbolising tombstones with ndk-stack.
"$TC/$HOST/bin/llvm-strip" -o "${SO%.so}.stripped.so" "$SO"
chmod 0644 "${SO%.so}.stripped.so"

echo "built $SO"
"$TC/$HOST/bin/llvm-readelf" -d "$SO" | grep -E 'NEEDED|SONAME|FLAGS'
"$TC/$HOST/bin/llvm-nm" -D --defined-only "$SO" | grep -E ' HMI$' || { echo "HMI not exported!" >&2; exit 1; }
ls -la "$SO"
