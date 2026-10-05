#!/usr/bin/env bash
#
# Does the driver see a real Windows boot?
#
# This is the first test that boots something other than the driver itself.
# The boot volume carries the UEFI shell, which loads the driver and then
# chainloads the Windows boot manager off a second disk. The driver hooks the
# firmware's image loader, so when that chainload happens the hook has to fire
# and the image has to be recognised.
#
# It is deliberately the smallest end to end check: earlier stages proved that
# the driver runs and that it can read a configuration. This one proves it is
# still running at the moment Windows starts, which is the assumption
# everything else rests on.
#
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

CHECK="boot_hook"
. tests/deploy/check.sh

BUILD_DIR="${BUILD_DIR:-build}"
SERIAL_LOG="${SERIAL_LOG:-$BUILD_DIR/boot-hook.log}"
WIN_DISK="${WIN_DISK:-}"

[ -n "$WIN_DISK" ] || checkSkip "WIN_DISK is not set"
[ -f "$WIN_DISK" ] || checkFail "no windows disk at $WIN_DISK"

# Loading and starting the Windows boot manager under emulation takes a while,
# so the timeout here is generous and the run stops as soon as the hook has
# reported what it saw.
WIN_DISK="$WIN_DISK" \
SERIAL_LOG="$SERIAL_LOG" \
STOP_PATTERN='loadimage: registered|loadimage: rejected|M4 failed' \
BOOT_TIMEOUT="${BOOT_TIMEOUT:-420}" \
    tests/deploy/run.sh

status=$?
if [ "$status" -ne 0 ]; then
    checkNote "last lines of the serial log:"
    tests/deploy/plain.sh "$SERIAL_LOG" | tail -5
    checkFail "the hook never reported an image"
fi

if ! tests/deploy/plain.sh "$SERIAL_LOG" | grep -q 'loadimage: registered'; then
    checkNote "what the hook said:"
    tests/deploy/plain.sh "$SERIAL_LOG" | grep -E 'loadimage' | tail -5
    checkFail "the hook fired but did not register the image"
fi

checkNote "what the hook said:"
tests/deploy/plain.sh "$SERIAL_LOG" | grep -E 'loadimage' | tail -5
checkPass "the hook fired and registered the boot manager"
