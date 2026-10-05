#!/usr/bin/env bash
#
# Does the plan the driver builds match the plan the host builds?
#
# The two are built by the same code, but over different things: the host tool
# reads files off disk, the driver reads images that a loader has already put
# into memory. Every way of getting that wrong - reading a file view as if the
# sections were laid out at their RVAs, or the other way round - shows up as a
# difference here and nowhere else.
#
# This is why the plan is printed at all. A fault later says only that
# something was wrong; this says which edit was decided on, and comparing it
# against the same decision made from the file isolates the reading from the
# deciding.
#
# PLAN_CORPUS has to point at one version's images, since the driver is booted
# against the same Windows build the host tool reads. Without it the check is
# skipped, like the others that need Windows media.
#
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

BUILD_DIR="${BUILD_DIR:-build}"
WORK="$BUILD_DIR/plan-case"
SERIAL_LOG="$WORK/plan.log"
WIN_DISK="${WIN_DISK:-}"
PLAN_CORPUS="${PLAN_CORPUS:-}"

if [ -z "$WIN_DISK" ]; then
    echo "WIN_DISK not set, skipping"
    exit 0
fi

if [ -z "$PLAN_CORPUS" ]; then
    echo "PLAN_CORPUS not set, skipping"
    exit 0
fi

if [ ! -f "$PLAN_CORPUS/winload.efi" ] || [ ! -f "$PLAN_CORPUS/ntoskrnl.exe" ]; then
    echo "PLAN_CORPUS has no winload.efi and ntoskrnl.exe: $PLAN_CORPUS" >&2
    exit 1
fi

mkdir -p "$WORK"

# The driver's own dump. It runs against the Windows volume that WIN_DISK
# names, so WIN_DISK and PLAN_CORPUS have to be the same build.
if ! ESP="$BUILD_DIR/esp.img" SERIAL_LOG="$SERIAL_LOG" \
     STOP_PATTERN='M5 planned|M5 incomplete' BOOT_TIMEOUT="${BOOT_TIMEOUT:-300}" \
     WIN_DISK="$WIN_DISK" tests/deploy/run.sh >"$WORK/plan.run" 2>&1; then
    echo "FAIL: the driver never produced a plan"
    tr -d '\r' <"$SERIAL_LOG" | grep -aE 'gmm|work|plan' | tail -8
    exit 1
fi

if grep -q 'M5 incomplete' "$SERIAL_LOG"; then
    echo "FAIL: the driver's plan is not complete"
    tr -d '\r' <"$SERIAL_LOG" | grep -a 'plan:' | tail -20
    exit 1
fi

target_plan="$WORK/target.plan"
host_plan="$WORK/host.plan"

# The target's lines, with the serial line endings taken off.
tr -d '\r' <"$SERIAL_LOG" | grep -a '^plan: ' >"$target_plan"

if [ ! -x tests/tools/planprobe ]; then
    make --no-print-directory -C tests/tools planprobe >/dev/null || {
        echo "FAIL: could not build the host tool"
        exit 1
    }
fi

if ! tests/tools/planprobe "$PLAN_CORPUS/winload.efi" "$PLAN_CORPUS/ntoskrnl.exe" \
        >"$host_plan" 2>"$WORK/host.err"; then
    echo "FAIL: the host tool could not build a plan"
    cat "$WORK/host.err"
    exit 1
fi

if [ ! -s "$target_plan" ]; then
    echo "FAIL: no plan lines in the serial log"
    exit 1
fi

if ! diff -u "$host_plan" "$target_plan" >"$WORK/plan.diff"; then
    echo "FAIL: the two plans differ"
    sed -n '1,40p' "$WORK/plan.diff"
    exit 1
fi

sites=$(grep -c '^plan: site ' "$target_plan")
echo "PASS: both plans agree ($sites sites, $(wc -l <"$target_plan") lines)"
