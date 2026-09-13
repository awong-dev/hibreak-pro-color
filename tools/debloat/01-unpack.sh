#!/bin/bash
# Extract the logical partitions from a super image.
# Upstream used `locate lpunpack` (needs a populated db) and deleted every
# *_b.img afterwards -- which threw away system_b, a real 57 MB ext4 holding
# system_other's dexopt staging. We keep it.
set -euo pipefail

SUPER="${1:-}"
[ -n "$SUPER" ] || { echo "usage: $0 <super.bin>"; exit 2; }
[ -f "$SUPER" ] || { echo "not a file: $SUPER"; exit 2; }

command -v lpunpack >/dev/null || { echo "lpunpack not on PATH"; exit 1; }

EXPECTED=12884901888
ACTUAL=$(stat -c %s "$SUPER")
if [ "$ACTUAL" != "$EXPECTED" ]; then
    echo "super is ${ACTUAL} bytes, expected ${EXPECTED}."
    echo "That check is telling you something -- do not just edit it out."
    exit 1
fi

lpunpack --slot=0 "$SUPER" .

# product_b / system_ext_b / vendor_b really are empty; system_b is not.
for f in product_b system_ext_b vendor_b; do
    [ -s "./${f}.img" ] || rm -f "./${f}.img"
done

echo "--- unpacked ---"
ls -l ./*.img
