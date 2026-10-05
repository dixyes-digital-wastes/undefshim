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

BUILD_DIR="${BUILD_DIR:-build}"
WORK="$BUILD_DIR/patch-case"
WIN_DISK="${WIN_DISK:-}"
# The entry point of the loader on the 26100 media the checks run against.
PATCH_RVA="${PATCH_RVA:-0x74c8}"
PATCH_TAG="${PATCH_TAG:-entry breakpoint}"

if [ -z "$WIN_DISK" ]; then
    echo "WIN_DISK not set, skipping"
    exit 0
fi

if [ ! -f "$WIN_DISK" ]; then
    echo "missing windows disk: $WIN_DISK" >&2
    exit 1
fi

mkdir -p "$WORK"
cat > "$WORK/patch.toml" <<EOF
version = 1

[log]
level = "info"

[debug]
enabled = true

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
    echo "FAIL: could not build the volume"
    cat "$WORK/patch.esp"
    exit 1
fi

if ! ESP="$WORK/patch.img" SERIAL_LOG="$log" WIN_DISK="$WIN_DISK" \
     STOP_PATTERN='M4 patched|M4 failed' BOOT_TIMEOUT="${BOOT_TIMEOUT:-420}" \
     tests/deploy/run.sh >"$WORK/patch.run" 2>&1; then
    echo "FAIL: the patch was never applied"
    grep -E 'loadimage|gmm|patch' "$log" | tail -8
    exit 1
fi

if ! grep -q "patch: $PATCH_TAG at " "$log"; then
    echo "FAIL: no patch report"
    grep -E 'patch: ' "$log" | tail -5
    exit 1
fi

base=$(sed -n 's/.*gmm: winload found at 0x\([0-9a-f]*\).*/\1/p' "$log" | head -1)
wrote=$(sed -n "s/.*patch: $PATCH_TAG at 0x\([0-9a-f]*\).*/\1/p" "$log" | head -1)
value=$(sed -n "s/.*patch: $PATCH_TAG at 0x[0-9a-f]* = \(0x[0-9a-f]*\)\/.*/\1/p" "$log" | head -1)

if [ -z "$base" ] || [ -z "$wrote" ]; then
    echo "FAIL: the reports are not parseable"
    grep -E 'gmm: winload found|patch: ' "$log" | tail -5
    exit 1
fi

# The addresses are hex, and bash reads bare digits as decimal.
expected=$(( 16#$base + PATCH_RVA ))
wroteDec=$(( 16#$wrote ))
if [ "$wroteDec" != "$expected" ]; then
    echo "FAIL: wrote at 0x$(printf %x "$wroteDec"), expected 0x$(printf %x "$expected")"
    echo "      image base 0x$base plus rva $PATCH_RVA"
    exit 1
fi

if [ "$value" != "0x14000000" ]; then
    echo "FAIL: reported value $value, expected 0x14000000"
    exit 1
fi

echo "PASS: winload base 0x$base, patched 0x$(printf %x "$expected") with $value"
