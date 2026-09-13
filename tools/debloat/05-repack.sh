#!/bin/bash
# Rebuild super from the (possibly modified) partition images.
#
# Differences from upstream repack.sh, all deliberate -- see README.md:
#   --virtual-ab                  our super's header declares virtual_ab_device
#   --group main_b:12882804736    stock gives main_b the same max as main_a,
#                                 not 0
#   system_b included             it is system_other (dexopt staging), 57 MB of
#                                 real ext4, not padding
#   product_b/system_ext_b/vendor_b declared at size 0, matching stock
set -euo pipefail

DEVICE_SIZE=12884901888     # from the GPT: super is exactly this
GROUP_MAX=12882804736       # from stock lpdump: main_a AND main_b
OUT=./super.new.bin

command -v lpmake >/dev/null || { echo "lpmake not on PATH"; exit 1; }
for p in product_a system_a system_ext_a vendor_a; do
    [ -f "./${p}.img" ] || { echo "missing ${p}.img"; exit 1; }
done
for d in system system_ext vendor product; do
    if mountpoint -q "./$d" 2>/dev/null; then
        echo "./$d is still mounted -- run 04-unmount.sh first"; exit 1
    fi
done

sz() { stat -c %s "$1"; }

ARGS=(
  --metadata-size 65536
  --super-name super
  --metadata-slots 3
  --virtual-ab
  --device "super:${DEVICE_SIZE}"
  --group "main_a:${GROUP_MAX}"
  --group "main_b:${GROUP_MAX}"
  # Declared in stock's order (a/b interleaved) so the resulting extent layout
  # matches too. liblp looks partitions up by name, so this is cosmetic -- but
  # "identical to stock" is a cheaper thing to defend than "equivalent to stock".
  --partition "product_a:readonly:$(sz ./product_a.img):main_a"
     --image product_a=./product_a.img
  --partition "product_b:readonly:0:main_b"
  --partition "system_a:readonly:$(sz ./system_a.img):main_a"
     --image system_a=./system_a.img
)
if [ -s ./system_b.img ]; then
    ARGS+=( --partition "system_b:readonly:$(sz ./system_b.img):main_b"
            --image system_b=./system_b.img )
else
    echo "note: no system_b.img -- declaring system_b empty"
    ARGS+=( --partition "system_b:readonly:0:main_b" )
fi
ARGS+=(
  --partition "system_ext_a:readonly:$(sz ./system_ext_a.img):main_a"
     --image system_ext_a=./system_ext_a.img
  --partition "system_ext_b:readonly:0:main_b"
  --partition "vendor_a:readonly:$(sz ./vendor_a.img):main_a"
     --image vendor_a=./vendor_a.img
  --partition "vendor_b:readonly:0:main_b"
)

rm -f "$OUT"
lpmake "${ARGS[@]}" --output "$OUT"

echo
echo "=== built $(stat -c %s "$OUT") bytes (device is ${DEVICE_SIZE}) ==="

# Verify against stock before anyone flashes this.
STOCK=/work/backup/out/super.bin
if command -v lpdump >/dev/null && [ -f "$STOCK" ]; then
    echo "=== metadata check: new vs stock ==="
    fail=0
    for key in "Metadata max size" "Metadata slot count" "Header flags"; do
        n=$(lpdump "$OUT"   2>/dev/null | grep -m1 "^$key" || true)
        s=$(lpdump "$STOCK" 2>/dev/null | grep -m1 "^$key" || true)
        if [ "$n" = "$s" ]; then printf '  OK    %s\n' "$n"
        else printf '  DIFF  new: %-40s stock: %s\n' "$n" "$s"; fail=1; fi
    done
    ng=$(lpdump "$OUT"   2>/dev/null | grep -c "Name: main_b" || true)
    echo "  main_b present in new image: $ng"
    if [ "$fail" != "0" ]; then
        echo
        echo "!! metadata differs from stock. DO NOT FLASH until you understand why."
        exit 1
    fi
    echo "  -> metadata matches stock"
fi

echo
echo "Flashing this is RED (CLAUDE.md 2.1). From the host:"
echo "  bin/mtk w super work/super/super.new.bin --preloader work/backup/out/preloader_boot1.bin"
