#!/usr/bin/env bash
#
# Boot a volume and check what ends up on the screen.
#
# Most of the checks read the serial port, but a machine whose configuration
# is missing or will not parse has no port to write to -- the driver only
# knows where the UART is once it has read a file that says so -- and the
# screen is then the only channel it has. This boots such a machine, watches
# the framebuffer for the line that is expected, and stops it again.
#
# The screen is read by screenread.py, whose text engine may not be installed.
# Without it the case is skipped, not failed: what a machine can check is not
# the same as what this one can.
#
#   screen_case.sh <esp> <qmp-port> <expected-text> [timeout]
#
# Exit: 0 the text appeared, 1 it did not, 2 the reader is unavailable.
#
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

CHECK="${CHECK:-screen_case}"
. tests/deploy/check.sh

ESP="$1"
QMP_PORT="$2"
EXPECT="$3"
TIMEOUT="${4:-150}"
WORK="${WORK:-build/screen-case}"

mkdir -p "$WORK"
log="$WORK/serial.log"
out="$WORK/run.log"
text="$WORK/screen.txt"

if ! command -v tesseract >/dev/null; then
    checkSkip "no tesseract, so the screen cannot be read"
fi

# The monitor has to outlive the boot, so the run is told not to stop on
# anything and is killed once the screen has been read
QMP_PORT="$QMP_PORT" KEEP=1 STOP_PATTERN='no such line' BOOT_TIMEOUT="$((TIMEOUT + 60))" \
ESP="$ESP" SERIAL_LOG="$log" WIN_DISK= \
    tests/deploy/run.sh >"$out" 2>&1 &
runner=$!

cleanup() {
    pkill -f "qemu-system-aarch64.*qmp tcp:127.0.0.1:$QMP_PORT" 2>/dev/null
    kill "$runner" 2>/dev/null
    wait "$runner" 2>/dev/null
}
trap cleanup EXIT INT TERM

if tests/deploy/screenread.py --qmp-port "$QMP_PORT" \
        --poll "$EXPECT" --timeout "$TIMEOUT" >"$text" 2>&1; then
    checkPass "the screen says \"$EXPECT\""
fi

checkNote "what it did say:"
cat "$text" 2>/dev/null || true
checkFail "the screen never said \"$EXPECT\""
