#!/usr/bin/env bash
#
# Build the volume the demos run from, for a WinPE boot.
#
# The guest reaches it as a virtio disk, and what Windows mounts without an
# argument is a partition table with a FAT32 partition in it: a bare FAT12 or
# FAT16 filesystem on a fixed disk, which is what "mformat" gives by default,
# is not mounted, and the image's own startnet.cmd then reports that it found
# no entrypoint.cmd on any drive.
#
#   demo_disk.sh <output.img> [size-mb]
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

OUT="${1:?usage: demo_disk.sh <output.img> [size-mb]}"
SIZE="${2:-64}"
DEMO="${DEMO_DIR:-$ROOT/tests/demo}"

for f in lrcpc_demo.exe udf_demo.exe; do
    [ -f "$DEMO/$f" ] || { echo "missing $DEMO/$f (run make demo)" >&2; exit 1; }
done

rm -f "$OUT"
dd if=/dev/zero of="$OUT" bs=1M count="$SIZE" status=none
# One primary partition, FAT32 LBA, starting at 1 MiB
sfdisk "$OUT" >/dev/null <<'TABLE'
label: dos
start=2048, type=0c, bootable
TABLE
mkfs.vfat -F 32 -n DEMO --offset=2048 "$OUT" >/dev/null
mcopy -i "$OUT@@1M" "$DEMO/lrcpc_demo.exe" "$DEMO/udf_demo.exe" \
      "$ROOT/tests/deploy/entrypoint.cmd" ::/
echo "demo disk: $OUT ($SIZE MB, FAT32)"
mdir -i "$OUT@@1M" :: | tail -4
