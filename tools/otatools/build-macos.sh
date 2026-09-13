#!/bin/bash
# Build lpunpack/lpmake/lpdump/lpadd/lpflash for macOS arm64 into tools/otatools/bin.
#
# Upstream make.sh does not build on a modern macOS toolchain. Three fixes, none
# of which are needed on Linux (the container builds it unpatched):
#
#   1. CFLAGS=-static          -- `-static` does not link on macOS.
#   2. liblog/event_tag_map.cpp uses std::unary_function, removed from current
#      libc++; re-enable it with the _LIBCPP_ENABLE_CXX17_REMOVED_* macros.
#   3. The bundled zlib's zutil.h sees TARGET_OS_MAC and does
#      `#define fdopen(fd,mode) NULL`, which then mangles the SDK's stdio.h
#      declaration of fdopen. `-Dfdopen=fdopen` makes its `#ifndef` guard false.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SRC="$ROOT/tools/otatools-src"
PIN=7ec860c   # LonelyFool/lpunpack_and_lpmake, 2021-11-15

if [ ! -d "$SRC" ]; then
  git clone --depth 1 https://github.com/LonelyFool/lpunpack_and_lpmake.git "$SRC"
fi
HEAD_SHA="$(git -C "$SRC" rev-parse --short HEAD)"
[ "$HEAD_SHA" = "$PIN" ] || echo "warning: otatools-src is at $HEAD_SHA, pinned build was $PIN" >&2

cd "$SRC"
sed -e 's/^CFLAGS=-static$/CFLAGS="-Wno-everything -D_LIBCPP_ENABLE_CXX17_REMOVED_UNARY_BINARY_FUNCTION -D_LIBCPP_ENABLE_CXX17_REMOVED_FEATURES"/' \
    -e 's|^\$CC -I\. -O3 -DHAVE_HIDDEN|$CC -I. -O3 -Dfdopen=fdopen -DHAVE_HIDDEN|' \
    make.sh > make-macos.sh
chmod +x make-macos.sh
./make-macos.sh

mkdir -p "$ROOT/tools/otatools/bin"
cp bin/* "$ROOT/tools/otatools/bin/"
echo "built: $(ls "$ROOT/tools/otatools/bin" | tr '\n' ' ')"
