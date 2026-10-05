#!/usr/bin/env bash
#
# Does the debug patch table actually write bytes into a real image?
#
# This is the check behind the milestone: name an image, name an RVA inside it,
# say what to write, and the byte has to land at that address. The console
# prints the address it wrote to, so the arithmetic (image base plus RVA) is
# verified here rather than being taken on trust.
#
# The place patched is the loader's entry point, and the bytes written are
# `b .` (0x14000000). If the run is left to continue past the patch, the loader
# spins at its own entry, which is exactly what a breakpoint means in gdb.
# The run is stopped as soon as the patch is reported, so a normal run does not
# depend on that.
#
# The Windows disk is external, so this is skipped unless WIN_DISK is set.
#
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

CHECK="patch_case"
. tests/deploy/check.sh

BUILD_DIR="${BUILD_DIR:-build}"
WORK="$BUILD_DIR/patch-case"
WIN_DISK="${WIN_DISK:-}"
# The entry point of the loader on the 26100 media the checks run against.
PATCH_RVA="${PATCH_RVA:-0x74c8}"
PATCH_TAG="${PATCH_TAG:-entry breakpoint}"

if [ -z "$WIN_DISK" ]; then
    checkSkip "WIN_DISK is not set"
fi

if [ ! -f "$WIN_DISK" ]; then
    checkFail "no windows disk at $WIN_DISK"
fi

mkdir -p "$WORK"
cat > "$WORK/patch.toml" <<EOF
version = 1

[uart]
# Where the machine's serial port is. Without this the driver has no port to
# report on, and a check that reads the log would be reading nothing
baseAddr = 0x09000000
type = "pl011"
width = 32
# A log with no colour escapes in it: a check reads these lines by
# matching text, and a colour sequence between the tag and the words
# is one more thing that can come between them and the pattern
color = false

[log]
level = "verbose"

[[debug.patch]]
target = "winload"
rva    = $PATCH_RVA
value  = 0x14000000
width  = 4
tag    = "$PATCH_TAG"
EOF

log="$WORK/patch.log"
if ! CONFIG="$WORK/patch.toml" ESP="$WORK/patch.img" tests/deploy/build_esp.sh \
        >"$WORK/patch.esp" 2>&1; then
    checkNote "the volume could not be built"
    cat "$WORK/patch.esp"
    exit 1
fi

if ! ESP="$WORK/patch.img" SERIAL_LOG="$log" WIN_DISK="$WIN_DISK" \
     STOP_PATTERN='M4 patched|M4 failed' BOOT_TIMEOUT="${BOOT_TIMEOUT:-420}" \
     tests/deploy/run.sh >"$WORK/patch.run" 2>&1; then
    checkNote "the patch was never applied"
    tests/deploy/plain.sh "$log" | grep -E 'loadimage|gmm|patch' | tail -8
    exit 1
fi

if ! tests/deploy/plain.sh "$log" | grep -q "patch: $PATCH_TAG at "; then
    checkNote "no patch report"
    tests/deploy/plain.sh "$log" | grep -E 'patch: ' | tail -5
    exit 1
fi

base=$(tests/deploy/plain.sh "$log" | sed -n 's/.*gmm: winload found at 0x\([0-9a-f]*\).*/\1/p' | head -1)
wrote=$(tests/deploy/plain.sh "$log" | sed -n "s/.*patch: $PATCH_TAG at 0x\([0-9a-f]*\).*/\1/p" | head -1)
value=$(tests/deploy/plain.sh "$log" | sed -n "s/.*patch: $PATCH_TAG at 0x[0-9a-f]* = \(0x[0-9a-f]*\)\/.*/\1/p" | head -1)

if [ -z "$base" ] || [ -z "$wrote" ]; then
    checkNote "the reports are not parseable"
    tests/deploy/plain.sh "$log" | grep -E 'gmm: winload found|patch: ' | tail -5
    exit 1
fi

# The addresses are hex, and bash reads bare digits as decimal.
expected=$(( 16#$base + PATCH_RVA ))
wroteDec=$(( 16#$wrote ))
if [ "$wroteDec" != "$expected" ]; then
    checkNote "wrote at 0x$(printf %x "$wroteDec"), expected 0x$(printf %x "$expected")"
    checkNote "image base 0x$base plus rva $PATCH_RVA"
fi

if [ "$value" != "0x14000000" ]; then
    checkNote "reported value $value, expected 0x14000000"
fi

checkPass "winload base 0x$base, patched 0x$(printf %x "$expected") with $value"
