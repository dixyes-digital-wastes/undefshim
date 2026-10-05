#!/usr/bin/env bash
#
# Which part of the driver breaks the boot manager.
#
# A whole-driver run faults at a fixed address a few seconds after the boot
# manager starts, and a run with no driver does not. This narrows that down by
# building a driver that does one thing and nothing else.
#
# The variant is compiled through DRIVER_MAIN, so the source tree is never
# rewritten. An earlier version of this script copied the variant over
# uefi/src/driver.c and restored it afterwards, which lost the real driver as
# soon as two runs overlapped.
#
# Each variant gets its own log: runs share the QEMU setup and a stale log from
# another variant is exactly the kind of mistake that wastes an afternoon.
# Runs are also serialised, because two guests at once would share the same
# serial socket and gdb port.
#
# Usage: tests/deploy/variant.sh <variant-file> <name>
#
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

VARIANT="${1:?usage: variant.sh <variant-file> <name>}"
NAME="${2:?usage: variant.sh <variant-file> <name>}"
WIN_DISK="${WIN_DISK:-../winemu/files/winpe_26100.qcow2}"
BUILD_DIR="${BUILD_DIR:-build}"
BOOT_TIMEOUT="${BOOT_TIMEOUT:-300}"
# The fault lands a few seconds into the boot manager. Watching from chainload
# for a fixed grace window keeps a clean run short without cutting a faulting
# run off early, which is what a plain overall timeout kept doing.
ARM_PATTERN="${ARM_PATTERN:-chainloading bootmgfw}"
GRACE="${GRACE:-90}"

VARIANT="$(realpath "$VARIANT")"

# A build directory of its own: a variant that shared one would leave its own
# driver_main.o and esp.img behind, and the next plain make would happily use
# them. The lock still lives in the shared directory, because what must not
# overlap is QEMU, not the build.
lock="$BUILD_DIR/variant.lock"
mkdir -p "$BUILD_DIR"
if ! mkdir "$lock" 2>/dev/null; then
    echo "$NAME: another variant run is in progress ($lock)" >&2
    exit 1
fi
trap 'rmdir "$lock" 2>/dev/null || true' EXIT INT TERM

BUILD_DIR="$BUILD_DIR/variant/$NAME"
if ! make BUILD_DIR="$BUILD_DIR" DRIVER_MAIN="$VARIANT" >/dev/null 2>&1; then
    echo "$NAME: BUILD FAILED"
    exit 1
fi
# BUILD_DIR is passed on explicitly: both scripts default it to the shared
# build directory, and an ESP assembled from the real driver would make every
# variant result meaningless.
BUILD_DIR="$BUILD_DIR" DRIVER="$BUILD_DIR/undefshim_driver.efi" \
ESP="$BUILD_DIR/esp.img" tests/deploy/build_esp.sh >/dev/null || {
    echo "$NAME: ESP BUILD FAILED"
    exit 1
}

log="$BUILD_DIR/$NAME.log"
out="$BUILD_DIR/$NAME.out"
rm -f "$log" "$out"

BUILD_DIR="$BUILD_DIR" ESP="$BUILD_DIR/esp.img" SERIAL_LOG="$log" \
WIN_DISK="$WIN_DISK" \
    STOP_PATTERN='variant-never-matches' ARM_PATTERN="$ARM_PATTERN" \
    GRACE="$GRACE" BOOT_TIMEOUT="$BOOT_TIMEOUT" \
    timeout $((BOOT_TIMEOUT + 60)) tests/deploy/run.sh >"$out" 2>&1

faults=$(tests/deploy/plain.sh "$log" | grep -c 'Synchronous' 2>/dev/null)
started=$(tests/deploy/plain.sh "$log" | grep -c 'M4 setup' 2>/dev/null)
chainload=$(tests/deploy/plain.sh "$log" | grep -c 'chainloading' 2>/dev/null)
reset=$(grep -c 'QEMU exited' "$out" 2>/dev/null)

: "${faults:=0}"
: "${started:=0}"
: "${chainload:=0}"
: "${reset:=0}"

if [ "$faults" -gt 0 ]; then
    verdict="FAULT"
elif [ "$reset" -gt 0 ]; then
    verdict="RESET (QEMU exited with no firmware exception reported)"
elif [ "$chainload" -gt 0 ]; then
    verdict="clean"
else
    verdict="INCONCLUSIVE (never reached the boot manager)"
fi

echo "$NAME: $verdict (started=$started chainload=$chainload faults=$faults)"
