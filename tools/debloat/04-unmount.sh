#!/bin/bash
# Unmount, fsck, shrink to minimum, fsck again.
set -euo pipefail
[ "$(id -u)" = "0" ] || { echo "needs root -- run inside bin/hibreak-shell"; exit 1; }

IMGS="system_ext system vendor product"

for p in $IMGS; do
    mountpoint -q "./$p" && umount -v "./$p"
done
for p in $IMGS; do
    e2fsck -fy "./${p}_a.img" || true     # e2fsck exits 1 when it fixed something
    resize2fs -M "./${p}_a.img"
    e2fsck -fy "./${p}_a.img" || true
done

echo "--- shrunk ---"
ls -l ./*_a.img
