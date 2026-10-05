#!/usr/bin/env bash
#
# The milestone's exit condition: a patch that a debugger can break on.
#
# The patch table writes `b .` - an infinite loop at that address - into the
# kernel's entry point. The boot then reaches the kernel and stops there, and
# the question this answers is whether the CPU is really executing the bytes we
# wrote.
#
# It is checked under gdb rather than from the serial log because the address
# the kernel ends up at is not the one the patch reported: the patch goes in at
# the identity mapped address the image was loaded at, and by the time the
# kernel runs the address space has been rebuilt, so the same code sits at a
# KASLR derived virtual address. Reading the instruction at the PC and finding
# our bytes there is what proves the patch survived all of that.
#
# The Windows disk is external, so this is skipped unless WIN_DISK is set.
#
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

CHECK="breakpoint_case"
. tests/deploy/check.sh

BUILD_DIR="${BUILD_DIR:-build}"
WORK="$BUILD_DIR/breakpoint-case"
WIN_DISK="${WIN_DISK:-}"
GDB="${GDB:-aarch64-linux-gnu-gdb}"
# Entry point of the kernel on the media the checks run against.
NTO_RVA="${NTO_RVA:-0xa5f7a0}"
# The kernel does not start until the address space has been rebuilt, so the
# machine is watched for a while after the patch lands before looking at it.
SETTLE="${SETTLE:-60}"

[ -f "$WIN_DISK" ] || checkSkip "WIN_DISK is not set or missing"
command -v "$GDB" >/dev/null || checkSkip "no $GDB to read the machine with"

mkdir -p "$WORK"
cat > "$WORK/bp.toml" <<EOF
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

[debug]
enabled = true

[[debug.patch]]
target = "ntoskrnl"
rva    = $NTO_RVA
value  = 0x14000000
width  = 4
tag    = "kernel entry breakpoint"
EOF

log="$WORK/bp.log"
CONFIG="$WORK/bp.toml" ESP="$WORK/bp.img" tests/deploy/build_esp.sh >"$WORK/bp.esp" 2>&1 || {
    checkNote "the volume could not be built"
    cat "$WORK/bp.esp"
    exit 1
}

# The run is not stopped by a marker: the machine has to stay up so gdb can be
# attached to it, and run.sh is what owns the QEMU process.
#
# Its own process group, so that everything it starts goes away with it. The
# pid the shell reports after a background command with an environment prefix
# is the subshell that prefix creates, not run.sh: killing it leaves the
# machine running, and the ports it holds are then the next check's problem
setsid env ESP="$WORK/bp.img" SERIAL_LOG="$log" WIN_DISK="$WIN_DISK" \
    STOP_PATTERN='breakpoint-never-matches' BOOT_TIMEOUT=$((SETTLE + 300)) \
    tests/deploy/run.sh >"$WORK/bp.run" 2>&1 &
runpid=$!

stop() {
    kill -TERM -"$runpid" 2>/dev/null || true
    sleep 1
    kill -KILL -"$runpid" 2>/dev/null || true
    # run.sh kills its own QEMU when it exits, but a run killed mid flight may
    # not get there; the ESP path makes this specific enough to be safe.
    pkill -f "file=$WORK/bp.img" 2>/dev/null || true
}
trap stop EXIT INT TERM

# Wait for the patch to be reported, then for the kernel to take over.
for _ in $(seq 1 200); do
    tests/deploy/plain.sh "$log" | grep -q 'M4 patched' 2>/dev/null && break
    sleep 1
done
if ! tests/deploy/plain.sh "$log" | grep -q 'M4 patched'; then
    checkNote "the patch was never applied"
    tests/deploy/plain.sh "$log" | grep -aE 'patch|gmm|milestone' | tail -5
    exit 1
fi

checkNote "the patch is applied; waiting ${SETTLE}s for the kernel to run"
sleep "$SETTLE"

gdbout="$WORK/bp.gdb"
timeout 60 "$GDB" -q -batch \
    -ex 'target remote :1234' \
    -ex 'thread 1' \
    -ex 'printf "PCVAL %lx\n", $pc' \
    -ex 'x/1xw $pc' \
    >"$gdbout" 2>&1

pc=$(sed -n 's/^PCVAL \([0-9a-f]*\)$/0x\1/p' "$gdbout" | head -1)

if [ -z "$pc" ] || [ "$pc" = "0x" ]; then
    checkNote "the program counter could not be read"
    tail -10 "$gdbout"
    exit 1
fi

# The word gdb printed at the PC has to be what the table asked for.
if ! grep -qP '^0x[0-9a-f]+:\s+0x14000000$' "$gdbout"; then
    checkNote "pc $pc does not hold the patch"
    grep -E '0x[0-9a-f]+:' "$gdbout" | head -2
    exit 1
fi

# And it has to be inside the kernel: the image base derived from the PC and
# the RVA has to be page aligned, which it is not for an address anywhere else.
pcval=$((pc))
rvaval=$((NTO_RVA))
base=$(( pcval - rvaval ))
if [ $(( base & 0xfff )) -ne 0 ]; then
    checkNote "pc $pc is not page aligned against the rva, base is $(printf 0x%x "$base")"
    exit 1
fi

case "$pc" in
0xffff*) ;;
*)
    checkNote "pc $pc is not in the higher half"
    exit 1
    ;;
esac

checkPass "the kernel is executing the patch at $pc (base $(printf 0x%x "$base"))"
