#!/usr/bin/env bash
#
# Build a bootable FAT image: the standalone UEFI shell as the boot loader,
# plus startup.nsh, the driver and the config.
#
# The firmware here has an internal shell, but it only falls back to it after
# every boot option fails, which costs minutes. Carrying the standalone shell
# as \EFI\BOOT\BOOTAA64.EFI boots straight into it instead.
#
# No root and no loop devices: mtools writes the filesystem in place.
#
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

BUILD_DIR="${BUILD_DIR:-build}"
DRIVER="${DRIVER:-$BUILD_DIR/undefshim_driver.efi}"
# An empty CONFIG builds a volume without one, which is how the "no config
# file anywhere" case is exercised.
CONFIG="${CONFIG-config/us.toml}"
# Which script the shell runs. A variant is useful for control runs that need
# to do something other than load the driver.
STARTUP="${STARTUP:-tests/deploy/startup.nsh}"
SHELL_EFI="${SHELL_EFI:-/usr/share/edk2-shell/aarch64/Shell.efi}"
ESP="${ESP:-$BUILD_DIR/esp.img}"
ESP_SIZE_MB="${ESP_SIZE_MB:-64}"

[ -f "$DRIVER" ] || { echo "missing driver: $DRIVER (run make first)" >&2; exit 1; }
[ -f "$SHELL_EFI" ] || { echo "missing shell: $SHELL_EFI" >&2; exit 1; }
[ -f "$STARTUP" ] || { echo "missing startup script: $STARTUP" >&2; exit 1; }
if [ -n "$CONFIG" ] && [ ! -f "$CONFIG" ]; then
    echo "missing config: $CONFIG" >&2
    exit 1
fi

rm -f "$ESP"
mkdir -p "$BUILD_DIR"
# A bare FAT volume, no partition table: firmware and shell both take it.
dd if=/dev/zero of="$ESP" bs=1M count="$ESP_SIZE_MB" status=none
mkfs.vfat -F 32 -n UNDEFSHIM "$ESP" >/dev/null

mmd -i "$ESP" ::/EFI ::/EFI/BOOT
mcopy -i "$ESP" "$SHELL_EFI" ::/EFI/BOOT/BOOTAA64.EFI
mcopy -i "$ESP" "$DRIVER" ::/undefshim_driver.efi
if [ -n "$CONFIG" ]; then
    mcopy -i "$ESP" "$CONFIG" ::/us.toml
# Lists live next to the configuration that names them, and go on the volume
# the same way: the driver resolves the directory relative to the volume it
# read the configuration from.
PATCHLIST_DIR="${PATCHLIST_DIR-}"
if [ -z "$PATCHLIST_DIR" ] && [ -d "$(dirname "$CONFIG")/usPatch" ]; then
    PATCHLIST_DIR="$(dirname "$CONFIG")/usPatch"
fi
if [ -d "$PATCHLIST_DIR" ]; then
    mmd -i "$ESP" ::/usPatch
    mcopy -i "$ESP" "$PATCHLIST_DIR"/* ::/usPatch/ || echo "esp: could not copy lists" >&2
fi
fi
mcopy -i "$ESP" "$STARTUP" ::/startup.nsh

echo "esp: $ESP ($(du -h "$ESP" | cut -f1))"
