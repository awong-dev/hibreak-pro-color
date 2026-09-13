#!/bin/bash
# Grow each image by 100 MB, resize the fs into the new space, and mount it.
# The slack is so files can be added; 04-unmount.sh shrinks it all back.
set -euo pipefail
[ "$(id -u)" = "0" ] || { echo "needs root -- run inside bin/hibreak-shell"; exit 1; }

IMGS="system system_ext vendor product"

for p in $IMGS; do
    [ -f "./${p}_a.img" ] || { echo "missing ${p}_a.img -- run 01-unpack.sh first"; exit 1; }
done

for p in $IMGS; do
    mkdir -p "./$p"
    dd if=/dev/zero bs=100M count=1 status=none >> "./${p}_a.img"
    resize2fs "./${p}_a.img"
    mount -t ext4 -o loop,rw "./${p}_a.img" "./$p"
    echo "mounted ${p}_a.img -> ./$p"
done
