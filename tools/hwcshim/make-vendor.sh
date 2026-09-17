#!/bin/bash
# Produce a vendor image carrying the HWC2 interposer, entirely on the macOS
# host with e2fsprogs (no Docker, no loop mount): grow the ext4 image, drop
# the .so in with debugfs, label it, shrink back.
#
#   tools/hwcshim/make-vendor.sh [src.img] [out.img]
#
# Defaults: src = work/super/vendor_a-xrzfix2.img (the property-context fix,
# patches/vendor-xrz-property-context.md), out = work/super/vendor_a-hwcshim.img.
#
# The shim is installed as /vendor/lib64/hw/hwcomposer.mtk_common.so. It wins
# over hwcomposer.mt6877.so because ro.hardware.hwcomposer=mtk_common is tried
# first by libhardware, so the original is left untouched -- reverting is
# `fastboot flash vendor work/super/vendor_a-xrzfix2.img`.
#
# Flashing the result is RED (CLAUDE.md §2.1); this script never touches the
# device.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
E2="/opt/homebrew/opt/e2fsprogs/sbin"
SRC="${1:-$ROOT/work/super/vendor_a-xrzfix2.img}"
OUT="${2:-$ROOT/work/super/vendor_a-hwcshim.img}"
SO="$ROOT/tools/hwcshim/out/hwcomposer.mtk_common.stripped.so"
DST="lib64/hw/hwcomposer.mtk_common.so"

[ -f "$SRC" ] || { echo "source image $SRC missing" >&2; exit 1; }
[ -f "$SO" ]  || { echo "$SO missing -- run tools/hwcshim/build.sh first" >&2; exit 1; }
for t in debugfs e2fsck resize2fs; do [ -x "$E2/$t" ] || { echo "$E2/$t missing (brew install e2fsprogs)" >&2; exit 1; }; done

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
printf 'u:object_r:vendor_file:s0\0' > "$TMP/ctx"

echo "== copy $SRC -> $OUT"
cp "$SRC" "$OUT"

echo "== grow by 8 MiB"
dd if=/dev/zero bs=1048576 count=8 status=none >> "$OUT"
"$E2/e2fsck" -fy "$OUT" >/dev/null || true
"$E2/resize2fs" "$OUT" >/dev/null

echo "== install $DST"
"$E2/debugfs" -w -R "rm $DST" "$OUT" >/dev/null 2>&1 || true
"$E2/debugfs" -w -R "write $SO $DST" "$OUT"
"$E2/debugfs" -w -R "sif $DST mode 0100644" "$OUT"
"$E2/debugfs" -w -R "sif $DST uid 0" "$OUT"
"$E2/debugfs" -w -R "sif $DST gid 0" "$OUT"
"$E2/debugfs" -w -R "ea_set -f $TMP/ctx $DST security.selinux" "$OUT"

echo "== check"
# Deliberately NOT shrunk back with `resize2fs -M`: that leaves ~16 KB free,
# and an in-place `cp` of a rebuilt shim over `adb` then truncates the file to
# zero and fails -- which makes the composer unable to load *any* HWC on its
# next restart. Keep the 8 MiB of headroom; the extra 8 MB on the flash is
# irrelevant for a logical partition.
"$E2/e2fsck" -fy "$OUT" >/dev/null || true
"$E2/e2fsck" -fn "$OUT" >/dev/null   # strict: must be clean now
"$E2/dumpe2fs" -h "$OUT" 2>/dev/null | grep -E 'Free blocks'


echo "== verify"
"$E2/debugfs" -R "ls -l lib64/hw" "$OUT" 2>/dev/null | grep -E 'hwcomposer'
"$E2/debugfs" -R "ea_list $DST" "$OUT" 2>/dev/null
"$E2/debugfs" -R "dump $DST $TMP/back.so" "$OUT" 2>/dev/null
if cmp -s "$SO" "$TMP/back.so"; then echo "round-trip OK: $(shasum -a 256 "$SO" | cut -c1-16)…"; else echo "ROUND-TRIP MISMATCH" >&2; exit 1; fi
# the original must be untouched
"$E2/debugfs" -R "dump lib64/hw/hwcomposer.mt6877.so $TMP/orig.so" "$OUT" 2>/dev/null
"$E2/debugfs" -R "dump lib64/hw/hwcomposer.mt6877.so $TMP/orig-src.so" "$SRC" 2>/dev/null
cmp -s "$TMP/orig.so" "$TMP/orig-src.so" && echo "hwcomposer.mt6877.so unchanged" || { echo "ORIGINAL HWC CHANGED" >&2; exit 1; }
ls -la "$OUT"
echo
echo "flash (RED, human runs):  fastboot reboot fastboot && fastboot flash vendor $OUT"
